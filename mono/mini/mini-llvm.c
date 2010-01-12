/*
 * mini-llvm.c: llvm "Backend" for the mono JIT
 *
 * (C) 2009 Novell, Inc.
 */

#include "mini.h"
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mempool-internals.h>

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include "llvm-c/Core.h"
#include "llvm-c/ExecutionEngine.h"
#include "llvm-c/BitWriter.h"
#include "llvm-c/Analysis.h"

#include "mini-llvm-cpp.h"

 /*
  * Information associated by mono with LLVM modules.
  */
typedef struct {
	LLVMModuleRef module;
	LLVMValueRef throw, throw_corlib_exception;
	GHashTable *llvm_types;
	LLVMValueRef got_var;
	const char *got_symbol;
	GHashTable *plt_entries;
} MonoLLVMModule;

/*
 * Information associated by the backend with mono basic blocks.
 */
typedef struct {
	LLVMBasicBlockRef bblock, end_bblock;
	LLVMValueRef finally_ind;
	gboolean added, invoke_target;
	/* 
	 * If this bblock is the start of a finally clause, this is a list of bblocks it
	 * needs to branch to in ENDFINALLY.
	 */
	GSList *call_handler_return_bbs;
	LLVMValueRef endfinally_switch;
	GSList *phi_nodes;
} BBInfo;

/*
 * Structure containing emit state
 */
typedef struct {
	MonoMemPool *mempool;

	/* Maps method names to the corresponding LLVMValueRef */
	GHashTable *emitted_method_decls;

	MonoCompile *cfg;
	LLVMValueRef lmethod;
	MonoLLVMModule *lmodule;
	LLVMModuleRef module;
	BBInfo *bblocks;
	int sindex, default_index, ex_index;
	LLVMBuilderRef builder;
	LLVMValueRef *values, *addresses;
	MonoType **vreg_cli_types;
	LLVMCallInfo *linfo;
	MonoMethodSignature *sig;
	GSList *builders;
	GHashTable *region_to_handler;
	LLVMBuilderRef alloca_builder;
	LLVMValueRef last_alloca;

	char temp_name [32];
} EmitContext;

typedef struct {
	MonoBasicBlock *bb;
	MonoInst *phi;
	MonoBasicBlock *in_bb;
	int sreg;
} PhiNode;

/*
 * Instruction metadata
 * This is the same as ins_info, but LREG != IREG.
 */
#ifdef MINI_OP
#undef MINI_OP
#endif
#ifdef MINI_OP3
#undef MINI_OP3
#endif
#define MINI_OP(a,b,dest,src1,src2) dest, src1, src2, ' ',
#define MINI_OP3(a,b,dest,src1,src2,src3) dest, src1, src2, src3,
#define NONE ' '
#define IREG 'i'
#define FREG 'f'
#define VREG 'v'
#define XREG 'x'
#define LREG 'l'
/* keep in sync with the enum in mini.h */
const char
llvm_ins_info[] = {
#include "mini-ops.h"
};
#undef MINI_OP
#undef MINI_OP3

#if SIZEOF_VOID_P == 4
#define GET_LONG_IMM(ins) (((guint64)(ins)->inst_ms_word << 32) | (guint64)(guint32)(ins)->inst_ls_word)
#else
#define GET_LONG_IMM(ins) ((ins)->inst_imm)
#endif

#define LLVM_INS_INFO(opcode) (&llvm_ins_info [((opcode) - OP_START - 1) * 4])

#if 0
#define TRACE_FAILURE(msg) do { printf ("%s\n", msg); } while (0)
#else
#define TRACE_FAILURE(msg)
#endif

#define LLVM_FAILURE(ctx, reason) do { \
	TRACE_FAILURE (reason); \
	(ctx)->cfg->exception_message = g_strdup (reason); \
	(ctx)->cfg->disable_llvm = TRUE; \
	goto FAILURE; \
} while (0)

#define CHECK_FAILURE(ctx) do { \
    if ((ctx)->cfg->disable_llvm) \
		goto FAILURE; \
} while (0)

static LLVMIntPredicate cond_to_llvm_cond [] = {
	LLVMIntEQ,
	LLVMIntNE,
	LLVMIntSLE,
	LLVMIntSGE,
	LLVMIntSLT,
	LLVMIntSGT,
	LLVMIntULE,
	LLVMIntUGE,
	LLVMIntULT,
	LLVMIntUGT,
};

static LLVMRealPredicate fpcond_to_llvm_cond [] = {
	LLVMRealOEQ,
	LLVMRealUNE,
	LLVMRealOLE,
	LLVMRealOGE,
	LLVMRealOLT,
	LLVMRealOGT,
	LLVMRealULE,
	LLVMRealUGE,
	LLVMRealULT,
	LLVMRealUGT,
};

static LLVMExecutionEngineRef ee;
static guint32 current_cfg_tls_id;

static MonoLLVMModule jit_module, aot_module;

/*
 * IntPtrType:
 *
 *   The LLVM type with width == sizeof (gpointer)
 */
static LLVMTypeRef
IntPtrType (void)
{
	return sizeof (gpointer) == 8 ? LLVMInt64Type () : LLVMInt32Type ();
}

/*
 * get_vtype_size:
 *
 *   Return the size of the LLVM representation of the vtype T.
 */
static guint32
get_vtype_size (MonoType *t)
{
	int size;

	size = mono_class_value_size (mono_class_from_mono_type (t), NULL);

	while (size < sizeof (gpointer) && mono_is_power_of_two (size) == -1)
		size ++;

	return size;
}

/*
 * simd_class_to_llvm_type:
 *
 *   Return the LLVM type corresponding to the Mono.SIMD class KLASS
 */
static LLVMTypeRef
simd_class_to_llvm_type (EmitContext *ctx, MonoClass *klass)
{
	if (!strcmp (klass->name, "Vector2d")) {
		return LLVMVectorType (LLVMDoubleType (), 2);
	} else if (!strcmp (klass->name, "Vector2l")) {
		return LLVMVectorType (LLVMInt64Type (), 2);
	} else if (!strcmp (klass->name, "Vector2ul")) {
		return LLVMVectorType (LLVMInt64Type (), 2);
	} else if (!strcmp (klass->name, "Vector4i")) {
		return LLVMVectorType (LLVMInt32Type (), 4);
	} else if (!strcmp (klass->name, "Vector4ui")) {
		return LLVMVectorType (LLVMInt32Type (), 4);
	} else if (!strcmp (klass->name, "Vector4f")) {
		return LLVMVectorType (LLVMFloatType (), 4);
	} else if (!strcmp (klass->name, "Vector8s")) {
		return LLVMVectorType (LLVMInt16Type (), 8);
	} else if (!strcmp (klass->name, "Vector8us")) {
		return LLVMVectorType (LLVMInt16Type (), 8);
	} else if (!strcmp (klass->name, "Vector16sb")) {
		return LLVMVectorType (LLVMInt8Type (), 16);
	} else if (!strcmp (klass->name, "Vector16b")) {
		return LLVMVectorType (LLVMInt8Type (), 16);
	} else {
		printf ("%s\n", klass->name);
		NOT_IMPLEMENTED;
		return NULL;
	}
}

/*
 * type_to_llvm_type:
 *
 *   Return the LLVM type corresponding to T.
 */
static LLVMTypeRef
type_to_llvm_type (EmitContext *ctx, MonoType *t)
{
	if (t->byref)
		return LLVMPointerType (LLVMInt8Type (), 0);
	switch (t->type) {
	case MONO_TYPE_VOID:
		return LLVMVoidType ();
	case MONO_TYPE_I1:
		return LLVMInt8Type ();
	case MONO_TYPE_I2:
		return LLVMInt16Type ();
	case MONO_TYPE_I4:
		return LLVMInt32Type ();
	case MONO_TYPE_U1:
		return LLVMInt8Type ();
	case MONO_TYPE_U2:
		return LLVMInt16Type ();
	case MONO_TYPE_U4:
		return LLVMInt32Type ();
	case MONO_TYPE_BOOLEAN:
		return LLVMInt8Type ();
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return LLVMInt64Type ();
	case MONO_TYPE_CHAR:
		return LLVMInt16Type ();
	case MONO_TYPE_R4:
		return LLVMFloatType ();
	case MONO_TYPE_R8:
		return LLVMDoubleType ();
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		return IntPtrType ();
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_STRING:
	case MONO_TYPE_PTR:
		return LLVMPointerType (IntPtrType (), 0);
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		/* Because of generic sharing */
		return IntPtrType ();
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return IntPtrType ();
		/* Fall through */
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_TYPEDBYREF: {
		MonoClass *klass;
		LLVMTypeRef ltype;

		klass = mono_class_from_mono_type (t);

		if (MONO_CLASS_IS_SIMD (ctx->cfg, klass))
			return simd_class_to_llvm_type (ctx, klass);

		if (klass->enumtype)
			return type_to_llvm_type (ctx, mono_class_enum_basetype (klass));
		ltype = g_hash_table_lookup (ctx->lmodule->llvm_types, klass);
		if (!ltype) {
			int i, size;
			LLVMTypeRef *eltypes;

			size = get_vtype_size (t);

			eltypes = g_new (LLVMTypeRef, size);
			for (i = 0; i < size; ++i)
				eltypes [i] = LLVMInt8Type ();

			ltype = LLVMStructType (eltypes, size, FALSE);
			g_hash_table_insert (ctx->lmodule->llvm_types, klass, ltype);
			g_free (eltypes);
		}
		return ltype;
	}

	default:
		printf ("X: %d\n", t->type);
		ctx->cfg->exception_message = g_strdup_printf ("type %s", mono_type_full_name (t));
		ctx->cfg->disable_llvm = TRUE;
		return NULL;
	}
}

/*
 * type_is_unsigned:
 *
 *   Return whenever T is an unsigned int type.
 */
static gboolean
type_is_unsigned (EmitContext *ctx, MonoType *t)
{
	if (t->byref)
		return FALSE;
	switch (t->type) {
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
		return TRUE;
	default:
		return FALSE;
	}
}

/*
 * type_to_llvm_arg_type:
 *
 *   Same as type_to_llvm_type, but treat i8/i16 as i32.
 */
static LLVMTypeRef
type_to_llvm_arg_type (EmitContext *ctx, MonoType *t)
{
	LLVMTypeRef ptype = type_to_llvm_type (ctx, t);
	
	if (ptype == LLVMInt8Type () || ptype == LLVMInt16Type ()) {
		/* 
		 * LLVM generates code which only sets the lower bits, while JITted
		 * code expects all the bits to be set.
		 */
		ptype = LLVMInt32Type ();
	}

	return ptype;
}

/*
 * llvm_type_to_stack_type:
 *
 *   Return the LLVM type which needs to be used when a value of type TYPE is pushed
 * on the IL stack.
 */
static G_GNUC_UNUSED LLVMTypeRef
llvm_type_to_stack_type (LLVMTypeRef type)
{
	if (type == NULL)
		return NULL;
	if (type == LLVMInt8Type ())
		return LLVMInt32Type ();
	else if (type == LLVMInt16Type ())
		return LLVMInt32Type ();
	else if (type == LLVMFloatType ())
		return LLVMDoubleType ();
	else
		return type;
}

/*
 * regtype_to_llvm_type:
 *
 *   Return the LLVM type corresponding to the regtype C used in instruction 
 * descriptions.
 */
static LLVMTypeRef
regtype_to_llvm_type (char c)
{
	switch (c) {
	case 'i':
		return LLVMInt32Type ();
	case 'l':
		return LLVMInt64Type ();
	case 'f':
		return LLVMDoubleType ();
	default:
		return NULL;
	}
}

/*
 * op_to_llvm_type:
 *
 *   Return the LLVM type corresponding to the unary/binary opcode OPCODE.
 */
static LLVMTypeRef
op_to_llvm_type (int opcode)
{
	switch (opcode) {
	case OP_ICONV_TO_I1:
	case OP_LCONV_TO_I1:
		return LLVMInt8Type ();
	case OP_ICONV_TO_U1:
	case OP_LCONV_TO_U1:
		return LLVMInt8Type ();
	case OP_ICONV_TO_I2:
	case OP_LCONV_TO_I2:
		return LLVMInt16Type ();
	case OP_ICONV_TO_U2:
	case OP_LCONV_TO_U2:
		return LLVMInt16Type ();
	case OP_ICONV_TO_I4:
	case OP_LCONV_TO_I4:
		return LLVMInt32Type ();
	case OP_ICONV_TO_U4:
	case OP_LCONV_TO_U4:
		return LLVMInt32Type ();
	case OP_ICONV_TO_I8:
		return LLVMInt64Type ();
	case OP_ICONV_TO_R4:
		return LLVMFloatType ();
	case OP_ICONV_TO_R8:
		return LLVMDoubleType ();
	case OP_ICONV_TO_U8:
		return LLVMInt64Type ();
	case OP_FCONV_TO_I4:
		return LLVMInt32Type ();
	case OP_FCONV_TO_I8:
		return LLVMInt64Type ();
	case OP_FCONV_TO_I1:
	case OP_FCONV_TO_U1:
		return LLVMInt8Type ();
	case OP_FCONV_TO_I2:
	case OP_FCONV_TO_U2:
		return LLVMInt16Type ();
	case OP_FCONV_TO_I:
	case OP_FCONV_TO_U:
		return sizeof (gpointer) == 8 ? LLVMInt64Type () : LLVMInt32Type ();
	case OP_IADD_OVF:
	case OP_IADD_OVF_UN:
	case OP_ISUB_OVF:
	case OP_ISUB_OVF_UN:
	case OP_IMUL_OVF:
	case OP_IMUL_OVF_UN:
		return LLVMInt32Type ();
	case OP_LADD_OVF:
	case OP_LADD_OVF_UN:
	case OP_LSUB_OVF:
	case OP_LSUB_OVF_UN:
	case OP_LMUL_OVF:
	case OP_LMUL_OVF_UN:
		return LLVMInt64Type ();
	default:
		printf ("%s\n", mono_inst_name (opcode));
		g_assert_not_reached ();
		return NULL;
	}
}		

/*
 * load_store_to_llvm_type:
 *
 *   Return the size/sign/zero extension corresponding to the load/store opcode
 * OPCODE.
 */
static LLVMTypeRef
load_store_to_llvm_type (int opcode, int *size, gboolean *sext, gboolean *zext)
{
	*sext = FALSE;
	*zext = FALSE;

	switch (opcode) {
	case OP_LOADI1_MEMBASE:
	case OP_STOREI1_MEMBASE_REG:
	case OP_STOREI1_MEMBASE_IMM:
		*size = 1;
		*sext = TRUE;
		return LLVMInt8Type ();
	case OP_LOADU1_MEMBASE:
	case OP_LOADU1_MEM:
		*size = 1;
		*zext = TRUE;
		return LLVMInt8Type ();
	case OP_LOADI2_MEMBASE:
	case OP_STOREI2_MEMBASE_REG:
	case OP_STOREI2_MEMBASE_IMM:
		*size = 2;
		*sext = TRUE;
		return LLVMInt16Type ();
	case OP_LOADU2_MEMBASE:
	case OP_LOADU2_MEM:
		*size = 2;
		*zext = TRUE;
		return LLVMInt16Type ();
	case OP_LOADI4_MEMBASE:
	case OP_LOADU4_MEMBASE:
	case OP_LOADI4_MEM:
	case OP_LOADU4_MEM:
	case OP_STOREI4_MEMBASE_REG:
	case OP_STOREI4_MEMBASE_IMM:
		*size = 4;
		return LLVMInt32Type ();
	case OP_LOADI8_MEMBASE:
	case OP_LOADI8_MEM:
	case OP_STOREI8_MEMBASE_REG:
	case OP_STOREI8_MEMBASE_IMM:
		*size = 8;
		return LLVMInt64Type ();
	case OP_LOADR4_MEMBASE:
	case OP_STORER4_MEMBASE_REG:
		*size = 4;
		return LLVMFloatType ();
	case OP_LOADR8_MEMBASE:
	case OP_STORER8_MEMBASE_REG:
		*size = 8;
		return LLVMDoubleType ();
	case OP_LOAD_MEMBASE:
	case OP_LOAD_MEM:
	case OP_STORE_MEMBASE_REG:
	case OP_STORE_MEMBASE_IMM:
		*size = sizeof (gpointer);
		return IntPtrType ();
	default:
		g_assert_not_reached ();
		return NULL;
	}
}

/*
 * ovf_op_to_intrins:
 *
 *   Return the LLVM intrinsics corresponding to the overflow opcode OPCODE.
 */
static const char*
ovf_op_to_intrins (int opcode)
{
	switch (opcode) {
	case OP_IADD_OVF:
		return "llvm.sadd.with.overflow.i32";
	case OP_IADD_OVF_UN:
		return "llvm.uadd.with.overflow.i32";
	case OP_ISUB_OVF:
		return "llvm.ssub.with.overflow.i32";
	case OP_ISUB_OVF_UN:
		return "llvm.usub.with.overflow.i32";
	case OP_IMUL_OVF:
		return "llvm.smul.with.overflow.i32";
	case OP_IMUL_OVF_UN:
		return "llvm.umul.with.overflow.i32";
	case OP_LADD_OVF:
		return "llvm.sadd.with.overflow.i64";
	case OP_LADD_OVF_UN:
		return "llvm.uadd.with.overflow.i64";
	case OP_LSUB_OVF:
		return "llvm.ssub.with.overflow.i64";
	case OP_LSUB_OVF_UN:
		return "llvm.usub.with.overflow.i64";
	case OP_LMUL_OVF:
		return "llvm.smul.with.overflow.i64";
	case OP_LMUL_OVF_UN:
		return "llvm.umul.with.overflow.i64";
	default:
		g_assert_not_reached ();
		return NULL;
	}
}

static const char*
simd_op_to_intrins (int opcode)
{
	switch (opcode) {
	case OP_MINPD:
		return "llvm.x86.sse2.min.pd";
	case OP_MINPS:
		return "llvm.x86.sse2.min.ps";
	case OP_PMIND_UN:
		return "llvm.x86.sse41.pminud";
	case OP_PMINW_UN:
		return "llvm.x86.sse41.pminuw";
	case OP_PMINB_UN:
		return "llvm.x86.sse41.pminub";
	case OP_MAXPD:
		return "llvm.x86.sse2.max.pd";
	case OP_MAXPS:
		return "llvm.x86.sse2.max.ps";
	case OP_PMAXD_UN:
		return "llvm.x86.sse41.pmaxud";
	case OP_PMAXW_UN:
		return "llvm.x86.sse41.pmaxuw";
	case OP_PMAXB_UN:
		return "llvm.x86.sse41.pmaxub";
	default:
		g_assert_not_reached ();
		return NULL;
	}
}

/*
 * get_bb:
 *
 *   Return the LLVM basic block corresponding to BB.
 */
static LLVMBasicBlockRef
get_bb (EmitContext *ctx, MonoBasicBlock *bb)
{
	char bb_name [128];

	if (ctx->bblocks [bb->block_num].bblock == NULL) {
		sprintf (bb_name, "BB%d", bb->block_num);

		ctx->bblocks [bb->block_num].bblock = LLVMAppendBasicBlock (ctx->lmethod, bb_name);
		ctx->bblocks [bb->block_num].end_bblock = ctx->bblocks [bb->block_num].bblock;
	}

	return ctx->bblocks [bb->block_num].bblock;
}

/* 
 * get_end_bb:
 *
 *   Return the last LLVM bblock corresponding to BB.
 * This might not be equal to the bb returned by get_bb () since we need to generate
 * multiple LLVM bblocks for a mono bblock to handle throwing exceptions.
 */
static LLVMBasicBlockRef
get_end_bb (EmitContext *ctx, MonoBasicBlock *bb)
{
	get_bb (ctx, bb);
	return ctx->bblocks [bb->block_num].end_bblock;
}

static LLVMBasicBlockRef
gen_bb (EmitContext *ctx, const char *prefix)
{
	char bb_name [128];

	sprintf (bb_name, "%s%d", prefix, ++ ctx->ex_index);
	return LLVMAppendBasicBlock (ctx->lmethod, bb_name);
}

/*
 * resolve_patch:
 *
 *   Return the target of the patch identified by TYPE and TARGET.
 */
static gpointer
resolve_patch (MonoCompile *cfg, MonoJumpInfoType type, gconstpointer target)
{
	MonoJumpInfo ji;

	memset (&ji, 0, sizeof (ji));
	ji.type = type;
	ji.data.target = target;

	return mono_resolve_patch_target (cfg->method, cfg->domain, NULL, &ji, FALSE);
}

/*
 * convert_full:
 *
 *   Emit code to convert the LLVM value V to DTYPE.
 */
static LLVMValueRef
convert_full (EmitContext *ctx, LLVMValueRef v, LLVMTypeRef dtype, gboolean is_unsigned)
{
	LLVMTypeRef stype = LLVMTypeOf (v);

	if (stype != dtype) {
		gboolean ext = FALSE;

		/* Extend */
		if (dtype == LLVMInt64Type () && (stype == LLVMInt32Type () || stype == LLVMInt16Type () || stype == LLVMInt8Type ()))
			ext = TRUE;
		else if (dtype == LLVMInt32Type () && (stype == LLVMInt16Type () || stype == LLVMInt8Type ()))
			ext = TRUE;
		else if (dtype == LLVMInt16Type () && (stype == LLVMInt8Type ()))
			ext = TRUE;

		if (ext)
			return is_unsigned ? LLVMBuildZExt (ctx->builder, v, dtype, "") : LLVMBuildSExt (ctx->builder, v, dtype, "");

		if (dtype == LLVMDoubleType () && stype == LLVMFloatType ())
			return LLVMBuildFPExt (ctx->builder, v, dtype, "");

		/* Trunc */
		if (stype == LLVMInt64Type () && (dtype == LLVMInt32Type () || dtype == LLVMInt16Type () || dtype == LLVMInt8Type ()))
			return LLVMBuildTrunc (ctx->builder, v, dtype, "");
		if (stype == LLVMInt32Type () && (dtype == LLVMInt16Type () || dtype == LLVMInt8Type ()))
			return LLVMBuildTrunc (ctx->builder, v, dtype, "");
		if (stype == LLVMDoubleType () && dtype == LLVMFloatType ())
			return LLVMBuildFPTrunc (ctx->builder, v, dtype, "");

		if (LLVMGetTypeKind (stype) == LLVMPointerTypeKind && LLVMGetTypeKind (dtype) == LLVMPointerTypeKind)
			return LLVMBuildBitCast (ctx->builder, v, dtype, "");
		if (LLVMGetTypeKind (dtype) == LLVMPointerTypeKind)
			return LLVMBuildIntToPtr (ctx->builder, v, dtype, "");
		if (LLVMGetTypeKind (stype) == LLVMPointerTypeKind)
			return LLVMBuildPtrToInt (ctx->builder, v, dtype, "");

#ifdef MONO_ARCH_SOFT_FLOAT
		if (stype == LLVMInt32Type () && dtype == LLVMFloatType ())
			return LLVMBuildBitCast (ctx->builder, v, dtype, "");
		if (stype == LLVMInt32Type () && dtype == LLVMDoubleType ())
			return LLVMBuildBitCast (ctx->builder, LLVMBuildZExt (ctx->builder, v, LLVMInt64Type (), ""), dtype, "");
#endif

		LLVMDumpValue (v);
		LLVMDumpValue (LLVMConstNull (dtype));
		g_assert_not_reached ();
		return NULL;
	} else {
		return v;
	}
}

static LLVMValueRef
convert (EmitContext *ctx, LLVMValueRef v, LLVMTypeRef dtype)
{
	return convert_full (ctx, v, dtype, FALSE);
}

/*
 * emit_volatile_load:
 *
 *   If vreg is volatile, emit a load from its address.
 */
static LLVMValueRef
emit_volatile_load (EmitContext *ctx, int vreg)
{
	MonoType *t;

	LLVMValueRef v = LLVMBuildLoad (ctx->builder, ctx->addresses [vreg], "");
	t = ctx->vreg_cli_types [vreg];
	if (t && !t->byref) {
		/* 
		 * Might have to zero extend since llvm doesn't have 
		 * unsigned types.
		 */
		if (t->type == MONO_TYPE_U1 || t->type == MONO_TYPE_U2)
			v = LLVMBuildZExt (ctx->builder, v, LLVMInt32Type (), "");
		else if (t->type == MONO_TYPE_U8)
			v = LLVMBuildZExt (ctx->builder, v, LLVMInt64Type (), "");
	}

	return v;
}

/*
 * emit_volatile_store:
 *
 *   If VREG is volatile, emit a store from its value to its address.
 */
static void
emit_volatile_store (EmitContext *ctx, int vreg)
{
	MonoInst *var = get_vreg_to_inst (ctx->cfg, vreg);

	if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT)) {
		g_assert (ctx->addresses [vreg]);
		LLVMBuildStore (ctx->builder, convert (ctx, ctx->values [vreg], type_to_llvm_type (ctx, var->inst_vtype)), ctx->addresses [vreg]);
	}
}

/*
 * sig_to_llvm_sig:
 *
 *   Return the LLVM signature corresponding to the mono signature SIG using the
 * calling convention information in CINFO.
 */
static LLVMTypeRef
sig_to_llvm_sig (EmitContext *ctx, MonoMethodSignature *sig, LLVMCallInfo *cinfo)
{
	LLVMTypeRef ret_type;
	LLVMTypeRef *param_types = NULL;
	LLVMTypeRef res;
	int i, j, pindex;
	gboolean vretaddr = FALSE;

	ret_type = type_to_llvm_type (ctx, sig->ret);
	CHECK_FAILURE (ctx);

	if (cinfo && cinfo->ret.storage == LLVMArgVtypeInReg) {
		/* LLVM models this by returning an aggregate value */
		if (cinfo->ret.pair_storage [0] == LLVMArgInIReg && cinfo->ret.pair_storage [1] == LLVMArgNone) {
			LLVMTypeRef members [2];

			members [0] = IntPtrType ();
			ret_type = LLVMStructType (members, 1, FALSE);
		} else {
			g_assert_not_reached ();
		}
	} else if (cinfo && MONO_TYPE_ISSTRUCT (sig->ret)) {
		g_assert (cinfo->ret.storage == LLVMArgVtypeRetAddr);
		vretaddr = TRUE;
	}

	param_types = g_new0 (LLVMTypeRef, (sig->param_count * 2) + 2);
	pindex = 0;
	if (vretaddr) {
		ret_type = LLVMVoidType ();
		param_types [pindex ++] = IntPtrType ();
	}
	if (sig->hasthis)
		param_types [pindex ++] = IntPtrType ();
	for (i = 0; i < sig->param_count; ++i) {
		if (cinfo && cinfo->args [i + sig->hasthis].storage == LLVMArgVtypeInReg) {
			for (j = 0; j < 2; ++j) {
				switch (cinfo->args [i + sig->hasthis].pair_storage [j]) {
				case LLVMArgInIReg:
					param_types [pindex ++] = LLVMIntType (sizeof (gpointer) * 8);
					break;
				case LLVMArgNone:
					break;
				default:
					g_assert_not_reached ();
				}
			}
		} else if (cinfo && cinfo->args [i + sig->hasthis].storage == LLVMArgVtypeByVal) {
			param_types [pindex] = type_to_llvm_arg_type (ctx, sig->params [i]);
			CHECK_FAILURE (ctx);
			param_types [pindex] = LLVMPointerType (param_types [pindex], 0);
			pindex ++;
		} else {
			param_types [pindex ++] = type_to_llvm_arg_type (ctx, sig->params [i]);
		}			
	}
	CHECK_FAILURE (ctx);

	res = LLVMFunctionType (ret_type, param_types, pindex, FALSE);
	g_free (param_types);

	return res;

 FAILURE:
	g_free (param_types);

	return NULL;
}

/*
 * LLVMFunctionType1:
 *
 *   Create an LLVM function type from the arguments.
 */
static G_GNUC_UNUSED LLVMTypeRef 
LLVMFunctionType1(LLVMTypeRef ReturnType,
				  LLVMTypeRef ParamType1,
				  int IsVarArg)
{
	LLVMTypeRef param_types [1];

	param_types [0] = ParamType1;

	return LLVMFunctionType (ReturnType, param_types, 1, IsVarArg);
}

/*
 * LLVMFunctionType2:
 *
 *   Create an LLVM function type from the arguments.
 */
static LLVMTypeRef 
LLVMFunctionType2(LLVMTypeRef ReturnType,
				  LLVMTypeRef ParamType1,
				  LLVMTypeRef ParamType2,
				  int IsVarArg)
{
	LLVMTypeRef param_types [2];

	param_types [0] = ParamType1;
	param_types [1] = ParamType2;

	return LLVMFunctionType (ReturnType, param_types, 2, IsVarArg);
}

/*
 * LLVMFunctionType3:
 *
 *   Create an LLVM function type from the arguments.
 */
static LLVMTypeRef 
LLVMFunctionType3(LLVMTypeRef ReturnType,
				  LLVMTypeRef ParamType1,
				  LLVMTypeRef ParamType2,
				  LLVMTypeRef ParamType3,
				  int IsVarArg)
{
	LLVMTypeRef param_types [3];

	param_types [0] = ParamType1;
	param_types [1] = ParamType2;
	param_types [2] = ParamType3;

	return LLVMFunctionType (ReturnType, param_types, 3, IsVarArg);
}

/*
 * create_builder:
 *
 *   Create an LLVM builder and remember it so it can be freed later.
 */
static LLVMBuilderRef
create_builder (EmitContext *ctx)
{
	LLVMBuilderRef builder = LLVMCreateBuilder ();

	ctx->builders = g_slist_prepend_mempool (ctx->cfg->mempool, ctx->builders, builder);

	return builder;
}

static LLVMValueRef
get_plt_entry (EmitContext *ctx, LLVMTypeRef llvm_sig, MonoJumpInfoType type, gconstpointer data)
{
	char *callee_name = mono_aot_get_plt_symbol (type, data);
	LLVMValueRef callee;

	if (!callee_name)
		return NULL;

	// FIXME: Locking
	callee = g_hash_table_lookup (ctx->lmodule->plt_entries, callee_name);
	if (!callee) {
		callee = LLVMAddFunction (ctx->module, callee_name, llvm_sig);

		g_hash_table_insert (ctx->lmodule->plt_entries, (char*)callee_name, callee);
	}

	return callee;
}

static void
emit_cond_throw_pos (EmitContext *ctx)
{
}

/*
 * emit_call:
 *
 *   Emit an LLVM call or invoke instruction depending on whenever the call is inside
 * a try region.
 */
static LLVMValueRef
emit_call (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, LLVMValueRef callee, LLVMValueRef *args, int pindex)
{
	MonoCompile *cfg = ctx->cfg;
	LLVMValueRef lcall;
	LLVMBuilderRef builder = *builder_ref;

	// FIXME: Nested clauses
	if (bb->region && MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_TRY)) {
		MonoMethodHeader *header = mono_method_get_header (cfg->method);
		// FIXME: Add a macro for this
		int clause_index = (bb->region >> 8) - 1;
		MonoExceptionClause *ec = &header->clauses [clause_index];
		MonoBasicBlock *tblock;
		LLVMBasicBlockRef ex_bb, noex_bb;

		/*
		 * Have to use an invoke instead of a call, branching to the
		 * handler bblock of the clause containing this bblock.
		 */

		g_assert (ec->flags == MONO_EXCEPTION_CLAUSE_NONE || ec->flags == MONO_EXCEPTION_CLAUSE_FINALLY);

		tblock = cfg->cil_offset_to_bb [ec->handler_offset];
		g_assert (tblock);

		ctx->bblocks [tblock->block_num].invoke_target = TRUE;

		ex_bb = get_bb (ctx, tblock);

		noex_bb = gen_bb (ctx, "NOEX_BB");

		/* Use an invoke */
		lcall = LLVMBuildInvoke (builder, callee, args, pindex, noex_bb, ex_bb, "");

		builder = ctx->builder = create_builder (ctx);
		LLVMPositionBuilderAtEnd (ctx->builder, noex_bb);

		ctx->bblocks [bb->block_num].end_bblock = noex_bb;
	} else {
		lcall = LLVMBuildCall (builder, callee, args, pindex, "");
		ctx->builder = builder;
	}

	*builder_ref = ctx->builder;

	return lcall;
}

/*
 * emit_cond_system_exception:
 *
 *   Emit code to throw the exception EXC_TYPE if the condition CMP is false.
 */
static void
emit_cond_system_exception (EmitContext *ctx, MonoBasicBlock *bb, const char *exc_type, LLVMValueRef cmp)
{
	LLVMBasicBlockRef ex_bb, noex_bb;
	LLVMBuilderRef builder;
	MonoClass *exc_class;
	LLVMValueRef args [2];

	ex_bb = gen_bb (ctx, "EX_BB");
	noex_bb = gen_bb (ctx, "NOEX_BB");

	LLVMBuildCondBr (ctx->builder, cmp, ex_bb, noex_bb);

	exc_class = mono_class_from_name (mono_defaults.corlib, "System", exc_type);
	g_assert (exc_class);

	/* Emit exception throwing code */
	builder = create_builder (ctx);
	LLVMPositionBuilderAtEnd (builder, ex_bb);

	if (!ctx->lmodule->throw_corlib_exception) {
		LLVMValueRef callee;
		LLVMTypeRef sig;

		MonoMethodSignature *throw_sig = mono_metadata_signature_alloc (mono_defaults.corlib, 2);
		throw_sig->ret = &mono_defaults.void_class->byval_arg;
		throw_sig->params [0] = &mono_defaults.int32_class->byval_arg;
		throw_sig->params [1] = &mono_defaults.int32_class->byval_arg;
		sig = sig_to_llvm_sig (ctx, throw_sig, NULL);

		if (ctx->cfg->compile_aot) {
			callee = get_plt_entry (ctx, sig, MONO_PATCH_INFO_INTERNAL_METHOD, "mono_arch_throw_corlib_exception");
		} else {
			callee = LLVMAddFunction (ctx->module, "throw_corlib_exception", sig_to_llvm_sig (ctx, throw_sig, NULL));
 
			LLVMAddGlobalMapping (ee, callee, resolve_patch (ctx->cfg, MONO_PATCH_INFO_INTERNAL_METHOD, "mono_arch_throw_corlib_exception"));
		}

		mono_memory_barrier ();
		ctx->lmodule->throw_corlib_exception = callee;
	}

	args [0] = LLVMConstInt (LLVMInt32Type (), exc_class->type_token, FALSE);
	/*
	 * FIXME: The offset is 0, this is not a problem for exception handling
	 * in general, because we don't llvm compile methods with handlers, its only
	 * a problem for line numbers in stack traces.
	 */
	args [1] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
	emit_call (ctx, bb, &builder, ctx->lmodule->throw_corlib_exception, args, 2);

	LLVMBuildUnreachable (builder);

	ctx->builder = create_builder (ctx);
	LLVMPositionBuilderAtEnd (ctx->builder, noex_bb);

	ctx->bblocks [bb->block_num].end_bblock = noex_bb;

	ctx->ex_index ++;
}

/*
 * emit_reg_to_vtype:
 *
 *   Emit code to store the vtype in the registers REGS to the address ADDRESS.
 */
static void
emit_reg_to_vtype (EmitContext *ctx, LLVMBuilderRef builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *regs)
{
	int j, size;

	size = get_vtype_size (t);

	if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type (t))) {
		address = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (LLVMInt8Type (), 0), "");
	}

	for (j = 0; j < 2; ++j) {
		LLVMValueRef index [2], addr;
		int part_size = size > sizeof (gpointer) ? sizeof (gpointer) : size;
		LLVMTypeRef part_type;

		if (ainfo->pair_storage [j] == LLVMArgNone)
			continue;

		part_type = LLVMIntType (part_size * 8);
		if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type (t))) {
			index [0] = LLVMConstInt (LLVMInt32Type (), j * sizeof (gpointer), FALSE);
			addr = LLVMBuildGEP (builder, address, index, 1, "");
		} else {
			index [0] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
			index [1] = LLVMConstInt (LLVMInt32Type (), j * sizeof (gpointer), FALSE);
			addr = LLVMBuildGEP (builder, address, index, 2, "");
		}
		switch (ainfo->pair_storage [j]) {
		case LLVMArgInIReg:
			LLVMBuildStore (builder, convert (ctx, regs [j], part_type), LLVMBuildBitCast (ctx->builder, addr, LLVMPointerType (part_type, 0), ""));
			break;
		case LLVMArgNone:
			break;
		default:
			g_assert_not_reached ();
		}

		size -= sizeof (gpointer);
	}
}

/*
 * emit_vtype_to_reg:
 *
 *   Emit code to load a vtype at address ADDRESS into registers. Store the registers
 * into REGS, and the number of registers into NREGS.
 */
static void
emit_vtype_to_reg (EmitContext *ctx, LLVMBuilderRef builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *regs, guint32 *nregs)
{
	int pindex = 0;
	int j, size;

	size = get_vtype_size (t);

	if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type (t))) {
		address = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (LLVMInt8Type (), 0), "");
	}

	for (j = 0; j < 2; ++j) {
		LLVMValueRef index [2], addr;
		int partsize = size > sizeof (gpointer) ? sizeof (gpointer) : size;

		if (ainfo->pair_storage [j] == LLVMArgNone)
			continue;

		if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type (t))) {
			index [0] = LLVMConstInt (LLVMInt32Type (), j * sizeof (gpointer), FALSE);
			addr = LLVMBuildGEP (builder, address, index, 1, "");
		} else {
			index [0] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
			index [1] = LLVMConstInt (LLVMInt32Type (), j * sizeof (gpointer), FALSE);				
			addr = LLVMBuildGEP (builder, address, index, 2, "");
		}
		switch (ainfo->pair_storage [j]) {
		case LLVMArgInIReg:
			regs [pindex ++] = convert (ctx, LLVMBuildLoad (builder, LLVMBuildBitCast (ctx->builder, addr, LLVMPointerType (LLVMIntType (partsize * 8), 0), ""), ""), IntPtrType ());
			break;
		case LLVMArgNone:
			break;
		default:
			g_assert_not_reached ();
		}
		size -= sizeof (gpointer);
	}

	*nregs = pindex;
}

static LLVMValueRef
build_alloca (EmitContext *ctx, MonoType *t)
{
	MonoClass *k = mono_class_from_mono_type (t);
	int align;

	if (MONO_CLASS_IS_SIMD (ctx->cfg, k))
		align = 16;
	else
		align = mono_class_min_align (k);

	/* Sometimes align is not a power of 2 */
	while (mono_is_power_of_two (align) == -1)
		align ++;

	/*
	 * Have to place all alloca's at the end of the entry bb, since otherwise they would
	 * get executed every time control reaches them.
	 */
	LLVMPositionBuilder (ctx->alloca_builder, get_bb (ctx, ctx->cfg->bb_entry), ctx->last_alloca);

	ctx->last_alloca = mono_llvm_build_alloca (ctx->alloca_builder, type_to_llvm_type (ctx, t), NULL, align, "");
	return ctx->last_alloca;
}

/*
 * Put the global into the 'llvm.used' array to prevent it from being optimized away.
 */
static void
mark_as_used (LLVMModuleRef module, LLVMValueRef global)
{
	LLVMTypeRef used_type;
	LLVMValueRef used, used_elem;
		
	used_type = LLVMArrayType (LLVMPointerType (LLVMInt8Type (), 0), 1);
	used = LLVMAddGlobal (module, used_type, "llvm.used");
	used_elem = LLVMConstBitCast (global, LLVMPointerType (LLVMInt8Type (), 0));
	LLVMSetInitializer (used, LLVMConstArray (LLVMPointerType (LLVMInt8Type (), 0), &used_elem, 1));
	LLVMSetLinkage (used, LLVMAppendingLinkage);
	LLVMSetSection (used, "llvm.metadata");
}

/*
 * emit_entry_bb:
 *
 *   Emit code to load/convert arguments.
 */
static void
emit_entry_bb (EmitContext *ctx, LLVMBuilderRef builder, int *pindexes)
{
	int i, pindex;
	MonoCompile *cfg = ctx->cfg;
	MonoMethodSignature *sig = ctx->sig;
	LLVMCallInfo *linfo = ctx->linfo;
	MonoBasicBlock *bb;

	ctx->alloca_builder = create_builder (ctx);

	/*
	 * Handle indirect/volatile variables by allocating memory for them
	 * using 'alloca', and storing their address in a temporary.
	 */
	for (i = 0; i < cfg->num_varinfo; ++i) {
		MonoInst *var = cfg->varinfo [i];
		LLVMTypeRef vtype;

		if (var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT) || MONO_TYPE_ISSTRUCT (var->inst_vtype)) {
			vtype = type_to_llvm_type (ctx, var->inst_vtype);
			CHECK_FAILURE (ctx);
			/* Could be already created by an OP_VPHI */
			if (!ctx->addresses [var->dreg])
				ctx->addresses [var->dreg] = build_alloca (ctx, var->inst_vtype);
			ctx->vreg_cli_types [var->dreg] = var->inst_vtype;
		}
	}

	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &linfo->args [i + sig->hasthis];
		int reg = cfg->args [i + sig->hasthis]->dreg;

		if (ainfo->storage == LLVMArgVtypeInReg) {
			LLVMValueRef regs [2];

			/* 
			 * Emit code to save the argument from the registers to 
			 * the real argument.
			 */
			pindex = pindexes [i];
			regs [0] = LLVMGetParam (ctx->lmethod, pindex);
			if (ainfo->pair_storage [1] != LLVMArgNone)
				regs [1] = LLVMGetParam (ctx->lmethod, pindex + 1);
			else
				regs [1] = NULL;

			ctx->addresses [reg] = build_alloca (ctx, sig->params [i]);

			emit_reg_to_vtype (ctx, builder, sig->params [i], ctx->addresses [reg], ainfo, regs);
		} else if (ainfo->storage == LLVMArgVtypeByVal) {
			ctx->addresses [reg] = LLVMGetParam (ctx->lmethod, pindexes [i]);
		} else {
			ctx->values [reg] = convert (ctx, ctx->values [reg], llvm_type_to_stack_type (type_to_llvm_type (ctx, sig->params [i])));
		}
	}

	if (cfg->vret_addr)
		emit_volatile_store (ctx, cfg->vret_addr->dreg);
	if (sig->hasthis)
		emit_volatile_store (ctx, cfg->args [0]->dreg);
	for (i = 0; i < sig->param_count; ++i)
		if (!MONO_TYPE_ISSTRUCT (sig->params [i]))
			emit_volatile_store (ctx, cfg->args [i + sig->hasthis]->dreg);

	/*
	 * For finally clauses, create an indicator variable telling OP_ENDFINALLY whenever
	 * it needs to continue normally, or return back to the exception handling system.
	 */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (bb->region != -1 && (bb->flags & BB_EXCEPTION_HANDLER))
			g_hash_table_insert (ctx->region_to_handler, GUINT_TO_POINTER (mono_get_block_region_notry (cfg, bb->region)), bb);
		if (bb->region != -1 && (bb->flags & BB_EXCEPTION_HANDLER) && bb->in_scount == 0) {
			LLVMValueRef val = LLVMBuildAlloca (builder, LLVMInt32Type (), "");
			LLVMBuildStore (builder, LLVMConstInt (LLVMInt32Type (), 0, FALSE), val);

			ctx->bblocks [bb->block_num].finally_ind = val;
		}
	}

 FAILURE:
	;
}

/* Have to export this for AOT */
void
mono_personality (void);
	
void
mono_personality (void)
{
	/* Not used */
	g_assert_not_reached ();
}

/*
 * mono_llvm_emit_method:
 *
 *   Emit LLVM IL from the mono IL, and compile it to native code using LLVM.
 */
void
mono_llvm_emit_method (MonoCompile *cfg)
{
	EmitContext *ctx;
	MonoMethodSignature *sig;
	MonoBasicBlock *bb;
	LLVMTypeRef method_type;
	LLVMValueRef method = NULL, debug_alias = NULL;
	char *method_name, *debug_name = NULL;
	LLVMValueRef *values, *addresses;
	LLVMTypeRef *vreg_types;
	MonoType **vreg_cli_types;
	int i, max_block_num, pindex, bb_index;
	int *pindexes = NULL;
	gboolean last = FALSE;
	GPtrArray *phi_values;
	LLVMCallInfo *linfo;
	GSList *l;
	LLVMModuleRef module;
	gboolean *is_dead;
	gboolean *unreachable;
	BBInfo *bblocks;
	GPtrArray *bblock_list;
	MonoMethodHeader *header;
	MonoExceptionClause *clause;

	/* The code below might acquire the loader lock, so use it for global locking */
	mono_loader_lock ();

	/* Used to communicate with the callbacks */
	TlsSetValue (current_cfg_tls_id, cfg);

	ctx = g_new0 (EmitContext, 1);
	ctx->cfg = cfg;
	ctx->mempool = cfg->mempool;

	/*
	 * This maps vregs to the LLVM instruction defining them
	 */
	values = g_new0 (LLVMValueRef, cfg->next_vreg);
	/*
	 * This maps vregs for volatile variables to the LLVM instruction defining their
	 * address.
	 */
	addresses = g_new0 (LLVMValueRef, cfg->next_vreg);
	vreg_types = g_new0 (LLVMTypeRef, cfg->next_vreg);
	vreg_cli_types = g_new0 (MonoType*, cfg->next_vreg);
	phi_values = g_ptr_array_new ();
	/* 
	 * This signals whenever the vreg was defined by a phi node with no input vars
	 * (i.e. all its input bblocks end with NOT_REACHABLE).
	 */
	is_dead = g_new0 (gboolean, cfg->next_vreg);
	/* Whenever the bblock is unreachable */
	unreachable = g_new0 (gboolean, cfg->max_block_num);

	bblock_list = g_ptr_array_new ();

	ctx->values = values;
	ctx->addresses = addresses;
	ctx->vreg_cli_types = vreg_cli_types;
	ctx->region_to_handler = g_hash_table_new (NULL, NULL);
 
	if (cfg->compile_aot) {
		ctx->lmodule = &aot_module;
		method_name = mono_aot_get_method_name (cfg);
		debug_name = mono_aot_get_method_debug_name (cfg);
	} else {
		ctx->lmodule = &jit_module;
		method_name = mono_method_full_name (cfg->method, TRUE);
		debug_name = NULL;
	}
	
	module = ctx->module = ctx->lmodule->module;

#if 1
	{
		static int count = 0;
		count ++;

		if (getenv ("LLVM_COUNT")) {
			if (count == atoi (getenv ("LLVM_COUNT"))) {
				printf ("LAST: %s\n", mono_method_full_name (cfg->method, TRUE));
				last = TRUE;
			}
			if (count > atoi (getenv ("LLVM_COUNT")))
				LLVM_FAILURE (ctx, "");
		}
	}
#endif

	sig = mono_method_signature (cfg->method);
	ctx->sig = sig;

	linfo = mono_arch_get_llvm_call_info (cfg, sig);
	ctx->linfo = linfo;
	CHECK_FAILURE (ctx);

	method_type = sig_to_llvm_sig (ctx, sig, linfo);
	CHECK_FAILURE (ctx);

	method = LLVMAddFunction (module, method_name, method_type);
	ctx->lmethod = method;

	LLVMSetLinkage (method, LLVMPrivateLinkage);

	if (cfg->method->save_lmf)
		LLVM_FAILURE (ctx, "lmf");

	if (sig->pinvoke)
		LLVM_FAILURE (ctx, "pinvoke signature");

	header = mono_method_get_header (cfg->method);
	for (i = 0; i < header->num_clauses; ++i) {
		clause = &header->clauses [i];
		if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY && clause->flags != MONO_EXCEPTION_CLAUSE_NONE)
			LLVM_FAILURE (ctx, "non-finally/catch clause.");
	}

	/* 
	 * This maps parameter indexes in the original signature to the indexes in
	 * the LLVM signature.
	 */
	pindexes = g_new0 (int, sig->param_count);
	pindex = 0;
	if (cfg->vret_addr) {
		values [cfg->vret_addr->dreg] = LLVMGetParam (method, pindex);
		pindex ++;
	}
	if (sig->hasthis) {
		values [cfg->args [0]->dreg] = LLVMGetParam (method, pindex);
		pindex ++;
	}
	for (i = 0; i < sig->param_count; ++i) {
		values [cfg->args [i + sig->hasthis]->dreg] = LLVMGetParam (method, pindex);
		pindexes [i] = pindex;
		if (linfo->args [i + sig->hasthis].storage == LLVMArgVtypeInReg) {
			if (linfo->args [i + sig->hasthis].pair_storage [0] != LLVMArgNone)
				pindex ++;
			if (linfo->args [i + sig->hasthis].pair_storage [1] != LLVMArgNone)
				pindex ++;
		} else if (linfo->args [i + sig->hasthis].storage == LLVMArgVtypeByVal) {
			LLVMAddAttribute (LLVMGetParam (method, pindex), LLVMByValAttribute);
			pindex ++;
		} else {
			pindex ++;
		}
	}

	max_block_num = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		max_block_num = MAX (max_block_num, bb->block_num);
	ctx->bblocks = bblocks = g_new0 (BBInfo, max_block_num + 1);

	/* Add branches between non-consecutive bblocks */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (bb->last_ins && MONO_IS_COND_BRANCH_OP (bb->last_ins) &&
			bb->next_bb != bb->last_ins->inst_false_bb) {
			
			MonoInst *inst = mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst));
			inst->opcode = OP_BR;
			inst->inst_target_bb = bb->last_ins->inst_false_bb;
			mono_bblock_add_inst (bb, inst);
		}
	}

	/*
	 * Make a first pass over the code to precreate PHI nodes.
	 */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		LLVMBuilderRef builder;
		char *dname;
		char dname_buf[128];

		builder = create_builder (ctx);

		for (ins = bb->code; ins; ins = ins->next) {
			switch (ins->opcode) {
			case OP_PHI:
			case OP_FPHI:
			case OP_VPHI:
			case OP_XPHI: {
				LLVMTypeRef phi_type = llvm_type_to_stack_type (type_to_llvm_type (ctx, &ins->klass->byval_arg));

				CHECK_FAILURE (ctx);

				if (ins->opcode == OP_VPHI) {
					/* Treat valuetype PHI nodes as operating on the address itself */
					g_assert (ins->klass);
					phi_type = LLVMPointerType (type_to_llvm_type (ctx, &ins->klass->byval_arg), 0);
				}

				/* 
				 * Have to precreate these, as they can be referenced by
				 * earlier instructions.
				 */
				sprintf (dname_buf, "t%d", ins->dreg);
				dname = dname_buf;
				values [ins->dreg] = LLVMBuildPhi (builder, phi_type, dname);

				if (ins->opcode == OP_VPHI)
					addresses [ins->dreg] = values [ins->dreg];

				g_ptr_array_add (phi_values, values [ins->dreg]);

				/* 
				 * Set the expected type of the incoming arguments since these have
				 * to have the same type.
				 */
				for (i = 0; i < ins->inst_phi_args [0]; i++) {
					int sreg1 = ins->inst_phi_args [i + 1];
					
					if (sreg1 != -1)
						vreg_types [sreg1] = phi_type;
				}
				break;
				}
			default:
				break;
			}
		}
	}

	/* 
	 * Create an ordering for bblocks, use the depth first order first, then
	 * put the exception handling bblocks last.
	 */
	for (bb_index = 0; bb_index < cfg->num_bblocks; ++bb_index) {
		bb = cfg->bblocks [bb_index];
		if (!(bb->region != -1 && !MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_TRY))) {
			g_ptr_array_add (bblock_list, bb);
			bblocks [bb->block_num].added = TRUE;
		}
	}

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (!bblocks [bb->block_num].added)
			g_ptr_array_add (bblock_list, bb);
	}

	/*
	 * Second pass: generate code.
	 */
	for (bb_index = 0; bb_index < bblock_list->len; ++bb_index) {
		MonoInst *ins;
		LLVMBasicBlockRef cbb;
		LLVMBuilderRef builder;
		gboolean has_terminator;
		LLVMValueRef v;
		LLVMValueRef lhs, rhs;

		bb = g_ptr_array_index (bblock_list, bb_index);

		if (!(bb == cfg->bb_entry || bb->in_count > 0))
			continue;

		cbb = get_bb (ctx, bb);
		builder = create_builder (ctx);
		ctx->builder = builder;
		LLVMPositionBuilderAtEnd (builder, cbb);

		if (bb == cfg->bb_entry)
			emit_entry_bb (ctx, builder, pindexes);
		CHECK_FAILURE (ctx);

		if (bb->flags & BB_EXCEPTION_HANDLER) {
			LLVMTypeRef i8ptr;
			LLVMValueRef eh_selector, eh_exception, personality, args [4];
			MonoInst *exvar;
			static gint32 mapping_inited;
			static int ti_generator;
			char ti_name [128];
			MonoClass **ti;
			LLVMValueRef type_info;
			int clause_index;

			if (!bblocks [bb->block_num].invoke_target) {
				/*
				 * LLVM asserts if llvm.eh.selector is called from a bblock which
				 * doesn't have an invoke pointing at it.
				 */
				LLVM_FAILURE (ctx, "handler without invokes");
			}

			eh_selector = LLVMGetNamedFunction (module, "llvm.eh.selector");

			if (cfg->compile_aot) {
				/* Use a dummy personality function */
				personality = LLVMGetNamedFunction (module, "mono_aot_personality");
				g_assert (personality);
			} else {
				personality = LLVMGetNamedFunction (module, "mono_personality");
				if (InterlockedCompareExchange (&mapping_inited, 1, 0) == 0)
					LLVMAddGlobalMapping (ee, personality, mono_personality);
			}

			i8ptr = LLVMPointerType (LLVMInt8Type (), 0);

			clause_index = (mono_get_block_region_notry (cfg, bb->region) >> 8) - 1;

			/*
			 * Create the type info
			 */
			sprintf (ti_name, "type_info_%d", ti_generator);
			ti_generator ++;

			if (cfg->compile_aot) {
				/* decode_eh_frame () in aot-runtime.c will decode this */
				type_info = LLVMAddGlobal (module, LLVMInt32Type (), ti_name);
				LLVMSetInitializer (type_info, LLVMConstInt (LLVMInt32Type (), clause_index, FALSE));

				/* 
				 * FIXME: llc currently generates incorrect data in the LSDA:
				 * 	.byte	0x9B                                        # @TType format (indirect pcrel sdata4)
				 * and later:
				 * .quad	type_info_1                                 # TypeInfo
				 */
				LLVM_FAILURE (ctx, "aot+clauses");
			} else {
				/* exception_cb will decode this */
				ti = g_malloc (sizeof (MonoExceptionClause));
				memcpy (ti, &mono_method_get_header (cfg->method)->clauses [clause_index], sizeof (MonoExceptionClause));

				type_info = LLVMAddGlobal (module, i8ptr, ti_name);

				LLVMAddGlobalMapping (ee, type_info, ti);
			}

			args [0] = LLVMConstNull (i8ptr);
			args [1] = LLVMConstBitCast (personality, i8ptr);
			args [2] = type_info;
			LLVMBuildCall (builder, eh_selector, args, 3, "");

			/* Store the exception into the exvar */
			if (bb->in_scount == 1) {
				g_assert (bb->in_scount == 1);
				exvar = bb->in_stack [0];

				eh_exception = LLVMGetNamedFunction (module, "llvm.eh.exception");

				// FIXME: This is shared with filter clauses ?
				g_assert (!values [exvar->dreg]);
				values [exvar->dreg] = LLVMBuildCall (builder, eh_exception, NULL, 0, "");
				emit_volatile_store (ctx, exvar->dreg);
			}
		}

		has_terminator = FALSE;
		for (ins = bb->code; ins; ins = ins->next) {
			const char *spec = LLVM_INS_INFO (ins->opcode);
			char *dname = NULL;
			char dname_buf [128];

			if (has_terminator)
				/* There could be instructions after a terminator, skip them */
				break;

			if (spec [MONO_INST_DEST] != ' ' && !MONO_IS_STORE_MEMBASE (ins)) {
				sprintf (dname_buf, "t%d", ins->dreg);
				dname = dname_buf;
			}

			if (spec [MONO_INST_SRC1] != ' ' && spec [MONO_INST_SRC1] != 'v') {
				MonoInst *var = get_vreg_to_inst (cfg, ins->sreg1);

				if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT)) {
					lhs = emit_volatile_load (ctx, ins->sreg1);
				} else {
					/* It is ok for SETRET to have an uninitialized argument */
					if (!values [ins->sreg1] && ins->opcode != OP_SETRET)
						LLVM_FAILURE (ctx, "sreg1");
					lhs = values [ins->sreg1];
				}
			} else {
				lhs = NULL;
			}

			if (spec [MONO_INST_SRC2] != ' ' && spec [MONO_INST_SRC2] != ' ') {
				MonoInst *var = get_vreg_to_inst (cfg, ins->sreg2);
				if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT)) {
					rhs = emit_volatile_load (ctx, ins->sreg2);
				} else {
					if (!values [ins->sreg2])
						LLVM_FAILURE (ctx, "sreg2");
					rhs = values [ins->sreg2];
				}
			} else {
				rhs = NULL;
			}

			//mono_print_ins (ins);
			switch (ins->opcode) {
			case OP_NOP:
			case OP_NOT_NULL:
			case OP_LIVERANGE_START:
			case OP_LIVERANGE_END:
				break;
			case OP_ICONST:
				values [ins->dreg] = LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE);
				break;
			case OP_I8CONST:
#if SIZEOF_VOID_P == 4
				values [ins->dreg] = LLVMConstInt (LLVMInt64Type (), GET_LONG_IMM (ins), FALSE);
#else
				values [ins->dreg] = LLVMConstInt (LLVMInt64Type (), (gint64)ins->inst_c0, FALSE);
#endif
				break;
			case OP_R8CONST:
				values [ins->dreg] = LLVMConstReal (LLVMDoubleType (), *(double*)ins->inst_p0);
				break;
			case OP_R4CONST:
				values [ins->dreg] = LLVMConstFPExt (LLVMConstReal (LLVMFloatType (), *(float*)ins->inst_p0), LLVMDoubleType ());
				break;
			case OP_BR:
				LLVMBuildBr (builder, get_bb (ctx, ins->inst_target_bb));
				has_terminator = TRUE;
				break;
			case OP_SWITCH: {
				int i;
				LLVMValueRef v;
				char bb_name [128];
				LLVMBasicBlockRef new_bb;
				LLVMBuilderRef new_builder;

				// The default branch is already handled
				// FIXME: Handle it here

				/* Start new bblock */
				sprintf (bb_name, "SWITCH_DEFAULT_BB%d", ctx->default_index ++);
				new_bb = LLVMAppendBasicBlock (ctx->lmethod, bb_name);

				lhs = convert (ctx, lhs, LLVMInt32Type ());
				v = LLVMBuildSwitch (builder, lhs, new_bb, GPOINTER_TO_UINT (ins->klass));
				for (i = 0; i < GPOINTER_TO_UINT (ins->klass); ++i) {
					MonoBasicBlock *target_bb = ins->inst_many_bb [i];

					LLVMAddCase (v, LLVMConstInt (LLVMInt32Type (), i, FALSE), get_bb (ctx, target_bb));
				}

				new_builder = create_builder (ctx);
				LLVMPositionBuilderAtEnd (new_builder, new_bb);
				LLVMBuildUnreachable (new_builder);

				has_terminator = TRUE;
				g_assert (!ins->next);
				
				break;
			}

			case OP_SETRET:
				if (linfo->ret.storage == LLVMArgVtypeInReg) {
					LLVMTypeRef ret_type = LLVMGetReturnType (LLVMGetElementType (LLVMTypeOf (method)));
					LLVMValueRef part1, retval;
					int size;

					size = get_vtype_size (sig->ret);

					g_assert (addresses [ins->sreg1]);

					g_assert (linfo->ret.pair_storage [0] == LLVMArgInIReg);
					g_assert (linfo->ret.pair_storage [1] == LLVMArgNone);
					
					part1 = convert (ctx, LLVMBuildLoad (builder, LLVMBuildBitCast (builder, addresses [ins->sreg1], LLVMPointerType (LLVMIntType (size * 8), 0), ""), ""), IntPtrType ());

					retval = LLVMBuildInsertValue (builder, LLVMGetUndef (ret_type), part1, 0, "");

					LLVMBuildRet (builder, retval);
					break;
				}

				if (linfo->ret.storage == LLVMArgVtypeRetAddr) {
					LLVMBuildRetVoid (builder);
					break;
				}

				if (!lhs || is_dead [ins->sreg1]) {
					/* 
					 * The method did not set its return value, probably because it
					 * ends with a throw.
					 */
					if (cfg->vret_addr)
						LLVMBuildRetVoid (builder);
					else
						LLVMBuildRet (builder, LLVMConstNull (type_to_llvm_type (ctx, sig->ret)));
				} else {
					LLVMBuildRet (builder, convert (ctx, lhs, type_to_llvm_type (ctx, sig->ret)));
				}
				has_terminator = TRUE;
				break;
			case OP_ICOMPARE:
			case OP_FCOMPARE:
			case OP_LCOMPARE:
			case OP_COMPARE:
			case OP_ICOMPARE_IMM:
			case OP_LCOMPARE_IMM:
			case OP_COMPARE_IMM:
#ifdef TARGET_AMD64
			case OP_AMD64_ICOMPARE_MEMBASE_REG:
			case OP_AMD64_ICOMPARE_MEMBASE_IMM:
#endif
#ifdef TARGET_X86
			case OP_X86_COMPARE_MEMBASE_REG:
			case OP_X86_COMPARE_MEMBASE_IMM:
#endif
			{
				CompRelation rel;
				LLVMValueRef cmp;

				if (ins->next->opcode == OP_NOP)
					break;

				if (ins->next->opcode == OP_BR)
					/* The comparison result is not needed */
					continue;

				rel = mono_opcode_to_cond (ins->next->opcode);

				/* Used for implementing bound checks */
#ifdef TARGET_AMD64
				if ((ins->opcode == OP_AMD64_ICOMPARE_MEMBASE_REG) || (ins->opcode == OP_AMD64_ICOMPARE_MEMBASE_IMM)) {
					int size = 4;
					LLVMValueRef index;
					LLVMTypeRef t;

					t = LLVMInt32Type ();

					g_assert (ins->inst_offset % size == 0);
					index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);				

					lhs = LLVMBuildLoad (builder, LLVMBuildGEP (builder, convert (ctx, values [ins->inst_basereg], LLVMPointerType (t, 0)), &index, 1, ""), "");
				}
				if (ins->opcode == OP_AMD64_ICOMPARE_MEMBASE_IMM) {
					lhs = convert (ctx, lhs, LLVMInt32Type ());
					rhs = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
				}
				if (ins->opcode == OP_AMD64_ICOMPARE_MEMBASE_REG)
					rhs = convert (ctx, rhs, LLVMInt32Type ());
#endif

#ifdef TARGET_X86
				if ((ins->opcode == OP_X86_COMPARE_MEMBASE_REG) || (ins->opcode == OP_X86_COMPARE_MEMBASE_IMM)) {
					int size = 4;
					LLVMValueRef index;
					LLVMTypeRef t;

					t = LLVMInt32Type ();

					g_assert (ins->inst_offset % size == 0);
					index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);				

					lhs = LLVMBuildLoad (builder, LLVMBuildGEP (builder, convert (ctx, values [ins->inst_basereg], LLVMPointerType (t, 0)), &index, 1, ""), "");
				}
				if (ins->opcode == OP_X86_COMPARE_MEMBASE_IMM) {
					lhs = convert (ctx, lhs, LLVMInt32Type ());
					rhs = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
				}
				if (ins->opcode == OP_X86_COMPARE_MEMBASE_REG)
					rhs = convert (ctx, rhs, LLVMInt32Type ());
#endif

				if (ins->opcode == OP_ICOMPARE_IMM) {
					lhs = convert (ctx, lhs, LLVMInt32Type ());
					rhs = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
				}
				if (ins->opcode == OP_LCOMPARE_IMM) {
					lhs = convert (ctx, lhs, LLVMInt64Type ());
					rhs = LLVMConstInt (LLVMInt64Type (), GET_LONG_IMM (ins), FALSE);
				}
				if (ins->opcode == OP_LCOMPARE) {
					lhs = convert (ctx, lhs, LLVMInt64Type ());
					rhs = convert (ctx, rhs, LLVMInt64Type ());
				}
				if (ins->opcode == OP_ICOMPARE) {
					lhs = convert (ctx, lhs, LLVMInt32Type ());
					rhs = convert (ctx, rhs, LLVMInt32Type ());
				}

				if (lhs && rhs) {
					if (LLVMGetTypeKind (LLVMTypeOf (lhs)) == LLVMPointerTypeKind)
						rhs = convert (ctx, rhs, LLVMTypeOf (lhs));
					else if (LLVMGetTypeKind (LLVMTypeOf (rhs)) == LLVMPointerTypeKind)
						lhs = convert (ctx, lhs, LLVMTypeOf (rhs));
				}

				/* We use COMPARE+SETcc/Bcc, llvm uses SETcc+br cond */
				if (ins->opcode == OP_FCOMPARE)
					cmp = LLVMBuildFCmp (builder, fpcond_to_llvm_cond [rel], convert (ctx, lhs, LLVMDoubleType ()), convert (ctx, rhs, LLVMDoubleType ()), "");
				else if (ins->opcode == OP_COMPARE_IMM)
					cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], convert (ctx, lhs, IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE), "");
				else if (ins->opcode == OP_COMPARE)
					cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], convert (ctx, lhs, IntPtrType ()), convert (ctx, rhs, IntPtrType ()), "");
				else
					cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], lhs, rhs, "");

				if (MONO_IS_COND_BRANCH_OP (ins->next)) {
					LLVMBuildCondBr (builder, cmp, get_bb (ctx, ins->next->inst_true_bb), get_bb (ctx, ins->next->inst_false_bb));
					has_terminator = TRUE;
				} else if (MONO_IS_SETCC (ins->next)) {
					sprintf (dname_buf, "t%d", ins->next->dreg);
					dname = dname_buf;
					values [ins->next->dreg] = LLVMBuildZExt (builder, cmp, LLVMInt32Type (), dname);

					/* Add stores for volatile variables */
					emit_volatile_store (ctx, ins->next->dreg);
				} else if (MONO_IS_COND_EXC (ins->next)) {
					//emit_cond_throw_pos (ctx);
					emit_cond_system_exception (ctx, bb, ins->next->inst_p1, cmp);
					builder = ctx->builder;
				} else {
					LLVM_FAILURE (ctx, "next");
				}

				ins = ins->next;
				break;
			}
			case OP_FCEQ:
			case OP_FCLT:
			case OP_FCLT_UN:
			case OP_FCGT:
			case OP_FCGT_UN: {
				CompRelation rel;
				LLVMValueRef cmp;

				rel = mono_opcode_to_cond (ins->opcode);

				cmp = LLVMBuildFCmp (builder, fpcond_to_llvm_cond [rel], convert (ctx, lhs, LLVMDoubleType ()), convert (ctx, rhs, LLVMDoubleType ()), "");
				values [ins->dreg] = LLVMBuildZExt (builder, cmp, LLVMInt32Type (), dname);
				break;
			}
			case OP_PHI:
			case OP_FPHI:
			case OP_VPHI:
			case OP_XPHI: {
				int i;
				gboolean empty = TRUE;

				/* Check that all input bblocks really branch to us */
				for (i = 0; i < bb->in_count; ++i) {
					if (bb->in_bb [i]->last_ins && bb->in_bb [i]->last_ins->opcode == OP_NOT_REACHED)
						ins->inst_phi_args [i + 1] = -1;
					else
						empty = FALSE;
				}

				if (empty) {
					/* LLVM doesn't like phi instructions with zero operands */
					is_dead [ins->dreg] = TRUE;
					break;
				}					

				/* Created earlier, insert it now */
				LLVMInsertIntoBuilder (builder, values [ins->dreg]);

				for (i = 0; i < ins->inst_phi_args [0]; i++) {
					int sreg1 = ins->inst_phi_args [i + 1];
					int count, j;

					/* 
					 * Count the number of times the incoming bblock branches to us,
					 * since llvm requires a separate entry for each.
					 */
					if (bb->in_bb [i]->last_ins && bb->in_bb [i]->last_ins->opcode == OP_SWITCH) {
						MonoInst *switch_ins = bb->in_bb [i]->last_ins;

						count = 0;
						for (j = 0; j < GPOINTER_TO_UINT (switch_ins->klass); ++j) {
							if (switch_ins->inst_many_bb [j] == bb)
								count ++;
						}
					} else {
						count = 1;
					}

					/* Remember for later */
					for (j = 0; j < count; ++j) {
						PhiNode *node = mono_mempool_alloc0 (ctx->mempool, sizeof (PhiNode));
						node->bb = bb;
						node->phi = ins;
						node->in_bb = bb->in_bb [i];
						node->sreg = sreg1;
						bblocks [bb->in_bb [i]->block_num].phi_nodes = g_slist_prepend_mempool (ctx->mempool, bblocks [bb->in_bb [i]->block_num].phi_nodes, node);
					}
				}
				break;
			}
			case OP_MOVE:
			case OP_LMOVE:
			case OP_XMOVE:
				g_assert (lhs);
				values [ins->dreg] = lhs;
				break;
			case OP_FMOVE: {
				MonoInst *var = get_vreg_to_inst (cfg, ins->dreg);
				
				g_assert (lhs);
				values [ins->dreg] = lhs;

				if (var && var->klass->byval_arg.type == MONO_TYPE_R4) {
					/* 
					 * This is added by the spilling pass in case of the JIT,
					 * but we have to do it ourselves.
					 */
					values [ins->dreg] = convert (ctx, values [ins->dreg], LLVMFloatType ());
				}
				break;
			}
			case OP_IADD:
			case OP_ISUB:
			case OP_IAND:
			case OP_IMUL:
			case OP_IDIV:
			case OP_IDIV_UN:
			case OP_IREM:
			case OP_IREM_UN:
			case OP_IOR:
			case OP_IXOR:
			case OP_ISHL:
			case OP_ISHR:
			case OP_ISHR_UN:
			case OP_FADD:
			case OP_FSUB:
			case OP_FMUL:
			case OP_FDIV:
			case OP_LADD:
			case OP_LSUB:
			case OP_LMUL:
			case OP_LDIV:
			case OP_LDIV_UN:
			case OP_LREM:
			case OP_LREM_UN:
			case OP_LAND:
			case OP_LOR:
			case OP_LXOR:
			case OP_LSHL:
			case OP_LSHR:
			case OP_LSHR_UN:
				lhs = convert (ctx, lhs, regtype_to_llvm_type (spec [MONO_INST_DEST]));
				rhs = convert (ctx, rhs, regtype_to_llvm_type (spec [MONO_INST_DEST]));

				switch (ins->opcode) {
				case OP_IADD:
				case OP_FADD:
				case OP_LADD:
					values [ins->dreg] = LLVMBuildAdd (builder, lhs, rhs, dname);
					break;
				case OP_ISUB:
				case OP_FSUB:
				case OP_LSUB:
					values [ins->dreg] = LLVMBuildSub (builder, lhs, rhs, dname);
					break;
				case OP_IMUL:
				case OP_FMUL:
				case OP_LMUL:
					values [ins->dreg] = LLVMBuildMul (builder, lhs, rhs, dname);
					break;
				case OP_IREM:
				case OP_LREM:
					values [ins->dreg] = LLVMBuildSRem (builder, lhs, rhs, dname);
					break;
				case OP_IREM_UN:
				case OP_LREM_UN:
					values [ins->dreg] = LLVMBuildURem (builder, lhs, rhs, dname);
					break;
				case OP_IDIV:
				case OP_LDIV:
					values [ins->dreg] = LLVMBuildSDiv (builder, lhs, rhs, dname);
					break;
				case OP_IDIV_UN:
				case OP_LDIV_UN:
					values [ins->dreg] = LLVMBuildUDiv (builder, lhs, rhs, dname);
					break;
				case OP_FDIV:
					values [ins->dreg] = LLVMBuildFDiv (builder, lhs, rhs, dname);
					break;
				case OP_IAND:
				case OP_LAND:
					values [ins->dreg] = LLVMBuildAnd (builder, lhs, rhs, dname);
					break;
				case OP_IOR:
				case OP_LOR:
					values [ins->dreg] = LLVMBuildOr (builder, lhs, rhs, dname);
					break;
				case OP_IXOR:
				case OP_LXOR:
					values [ins->dreg] = LLVMBuildXor (builder, lhs, rhs, dname);
					break;
				case OP_ISHL:
				case OP_LSHL:
					values [ins->dreg] = LLVMBuildShl (builder, lhs, rhs, dname);
					break;
				case OP_ISHR:
				case OP_LSHR:
					values [ins->dreg] = LLVMBuildAShr (builder, lhs, rhs, dname);
					break;
				case OP_ISHR_UN:
				case OP_LSHR_UN:
					values [ins->dreg] = LLVMBuildLShr (builder, lhs, rhs, dname);
					break;
				default:
					g_assert_not_reached ();
				}
				break;
			case OP_IADD_IMM:
			case OP_ISUB_IMM:
			case OP_IMUL_IMM:
			case OP_IREM_IMM:
			case OP_IREM_UN_IMM:
			case OP_IDIV_IMM:
			case OP_IDIV_UN_IMM:
			case OP_IAND_IMM:
			case OP_IOR_IMM:
			case OP_IXOR_IMM:
			case OP_ISHL_IMM:
			case OP_ISHR_IMM:
			case OP_ISHR_UN_IMM:
			case OP_LADD_IMM:
			case OP_LSUB_IMM:
			case OP_LREM_IMM:
			case OP_LAND_IMM:
			case OP_LOR_IMM:
			case OP_LXOR_IMM:
			case OP_LSHL_IMM:
			case OP_LSHR_IMM:
			case OP_LSHR_UN_IMM:
			case OP_ADD_IMM:
			case OP_AND_IMM:
			case OP_MUL_IMM:
			case OP_SHL_IMM:
			case OP_SHR_IMM: {
				LLVMValueRef imm;

				if (spec [MONO_INST_SRC1] == 'l') {
					imm = LLVMConstInt (LLVMInt64Type (), GET_LONG_IMM (ins), FALSE);
				} else {
					imm = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
				}

#if SIZEOF_VOID_P == 4
				if (ins->opcode == OP_LSHL_IMM || ins->opcode == OP_LSHR_IMM || ins->opcode == OP_LSHR_UN_IMM)
					imm = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
#endif

				if (LLVMGetTypeKind (LLVMTypeOf (lhs)) == LLVMPointerTypeKind)
					lhs = convert (ctx, lhs, IntPtrType ());
				imm = convert (ctx, imm, LLVMTypeOf (lhs));
				switch (ins->opcode) {
				case OP_IADD_IMM:
				case OP_LADD_IMM:
				case OP_ADD_IMM:
					values [ins->dreg] = LLVMBuildAdd (builder, lhs, imm, dname);
					break;
				case OP_ISUB_IMM:
				case OP_LSUB_IMM:
					values [ins->dreg] = LLVMBuildSub (builder, lhs, imm, dname);
					break;
				case OP_IMUL_IMM:
				case OP_MUL_IMM:
					values [ins->dreg] = LLVMBuildMul (builder, lhs, imm, dname);
					break;
				case OP_IDIV_IMM:
				case OP_LDIV_IMM:
					values [ins->dreg] = LLVMBuildSDiv (builder, lhs, imm, dname);
					break;
				case OP_IDIV_UN_IMM:
				case OP_LDIV_UN_IMM:
					values [ins->dreg] = LLVMBuildUDiv (builder, lhs, imm, dname);
					break;
				case OP_IREM_IMM:
				case OP_LREM_IMM:
					values [ins->dreg] = LLVMBuildSRem (builder, lhs, imm, dname);
					break;
				case OP_IREM_UN_IMM:
					values [ins->dreg] = LLVMBuildURem (builder, lhs, imm, dname);
					break;
				case OP_IAND_IMM:
				case OP_LAND_IMM:
				case OP_AND_IMM:
					values [ins->dreg] = LLVMBuildAnd (builder, lhs, imm, dname);
					break;
				case OP_IOR_IMM:
				case OP_LOR_IMM:
					values [ins->dreg] = LLVMBuildOr (builder, lhs, imm, dname);
					break;
				case OP_IXOR_IMM:
				case OP_LXOR_IMM:
					values [ins->dreg] = LLVMBuildXor (builder, lhs, imm, dname);
					break;
				case OP_ISHL_IMM:
				case OP_LSHL_IMM:
				case OP_SHL_IMM:
					values [ins->dreg] = LLVMBuildShl (builder, lhs, imm, dname);
					break;
				case OP_ISHR_IMM:
				case OP_LSHR_IMM:
				case OP_SHR_IMM:
					values [ins->dreg] = LLVMBuildAShr (builder, lhs, imm, dname);
					break;
				case OP_ISHR_UN_IMM:
					/* This is used to implement conv.u4, so the lhs could be an i8 */
					lhs = convert (ctx, lhs, LLVMInt32Type ());
					imm = convert (ctx, imm, LLVMInt32Type ());
					values [ins->dreg] = LLVMBuildLShr (builder, lhs, imm, dname);
					break;
				case OP_LSHR_UN_IMM:
					values [ins->dreg] = LLVMBuildLShr (builder, lhs, imm, dname);
					break;
				default:
					g_assert_not_reached ();
				}
				break;
			}
			case OP_INEG:
				values [ins->dreg] = LLVMBuildSub (builder, LLVMConstInt (LLVMInt32Type (), 0, FALSE), convert (ctx, lhs, LLVMInt32Type ()), dname);
				break;
			case OP_LNEG:
				values [ins->dreg] = LLVMBuildSub (builder, LLVMConstInt (LLVMInt64Type (), 0, FALSE), lhs, dname);
				break;
			case OP_FNEG:
				lhs = convert (ctx, lhs, LLVMDoubleType ());
				values [ins->dreg] = LLVMBuildSub (builder, LLVMConstReal (LLVMDoubleType (), 0.0), lhs, dname);
				break;
			case OP_INOT: {
				guint32 v = 0xffffffff;
				values [ins->dreg] = LLVMBuildXor (builder, LLVMConstInt (LLVMInt32Type (), v, FALSE), lhs, dname);
				break;
			}
			case OP_LNOT: {
				guint64 v = 0xffffffffffffffffLL;
				values [ins->dreg] = LLVMBuildXor (builder, LLVMConstInt (LLVMInt64Type (), v, FALSE), lhs, dname);
				break;
			}
#if defined(TARGET_X86) || defined(TARGET_AMD64)
			case OP_X86_LEA: {
				LLVMValueRef v1, v2;

				v1 = LLVMBuildMul (builder, convert (ctx, rhs, IntPtrType ()), LLVMConstInt (IntPtrType (), (1 << ins->backend.shift_amount), FALSE), "");
				v2 = LLVMBuildAdd (builder, convert (ctx, lhs, IntPtrType ()), v1, "");
				values [ins->dreg] = LLVMBuildAdd (builder, v2, LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE), dname);
				break;
			}
#endif

			case OP_ICONV_TO_I1:
			case OP_ICONV_TO_I2:
			case OP_ICONV_TO_I4:
			case OP_ICONV_TO_U1:
			case OP_ICONV_TO_U2:
			case OP_ICONV_TO_U4:
			case OP_LCONV_TO_I1:
			case OP_LCONV_TO_I2:
			case OP_LCONV_TO_U1:
			case OP_LCONV_TO_U2:
			case OP_LCONV_TO_U4: {
				gboolean sign;

				sign = (ins->opcode == OP_ICONV_TO_I1) || (ins->opcode == OP_ICONV_TO_I2) || (ins->opcode == OP_ICONV_TO_I4) || (ins->opcode == OP_LCONV_TO_I1) || (ins->opcode == OP_LCONV_TO_I2);

				/* Have to do two casts since our vregs have type int */
				v = LLVMBuildTrunc (builder, lhs, op_to_llvm_type (ins->opcode), "");
				if (sign)
					values [ins->dreg] = LLVMBuildSExt (builder, v, LLVMInt32Type (), dname);
				else
					values [ins->dreg] = LLVMBuildZExt (builder, v, LLVMInt32Type (), dname);
				break;
			}
			case OP_ICONV_TO_I8:
				values [ins->dreg] = LLVMBuildSExt (builder, lhs, LLVMInt64Type (), dname);
				break;
			case OP_ICONV_TO_U8:
				values [ins->dreg] = LLVMBuildZExt (builder, lhs, LLVMInt64Type (), dname);
				break;
			case OP_FCONV_TO_I4:
				values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, LLVMInt32Type (), dname);
				break;
			case OP_FCONV_TO_I1:
				values [ins->dreg] = LLVMBuildSExt (builder, LLVMBuildFPToSI (builder, lhs, LLVMInt8Type (), dname), LLVMInt32Type (), "");
				break;
			case OP_FCONV_TO_U1:
				values [ins->dreg] = LLVMBuildZExt (builder, LLVMBuildFPToUI (builder, lhs, LLVMInt8Type (), dname), LLVMInt32Type (), "");
				break;
			case OP_FCONV_TO_I2:
				values [ins->dreg] = LLVMBuildSExt (builder, LLVMBuildFPToSI (builder, lhs, LLVMInt16Type (), dname), LLVMInt32Type (), "");
				break;
			case OP_FCONV_TO_U2:
				values [ins->dreg] = LLVMBuildZExt (builder, LLVMBuildFPToUI (builder, lhs, LLVMInt16Type (), dname), LLVMInt32Type (), "");
				break;
			case OP_FCONV_TO_I8:
				values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, LLVMInt64Type (), dname);
				break;
			case OP_FCONV_TO_I:
				values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, IntPtrType (), dname);
				break;
			case OP_ICONV_TO_R8:
			case OP_LCONV_TO_R8:
				values [ins->dreg] = LLVMBuildSIToFP (builder, lhs, LLVMDoubleType (), dname);
				break;
			case OP_LCONV_TO_R_UN:
				values [ins->dreg] = LLVMBuildUIToFP (builder, lhs, LLVMDoubleType (), dname);
				break;
#if SIZEOF_VOID_P == 4
			case OP_LCONV_TO_U:
#endif
			case OP_LCONV_TO_I4:
				values [ins->dreg] = LLVMBuildTrunc (builder, lhs, LLVMInt32Type (), dname);
				break;
			case OP_ICONV_TO_R4:
			case OP_LCONV_TO_R4:
				v = LLVMBuildSIToFP (builder, lhs, LLVMFloatType (), "");
				values [ins->dreg] = LLVMBuildFPExt (builder, v, LLVMDoubleType (), dname);
				break;
			case OP_FCONV_TO_R4:
				v = LLVMBuildFPTrunc (builder, lhs, LLVMFloatType (), "");
				values [ins->dreg] = LLVMBuildFPExt (builder, v, LLVMDoubleType (), dname);
				break;
			case OP_SEXT_I4:
				values [ins->dreg] = LLVMBuildSExt (builder, lhs, LLVMInt64Type (), dname);
				break;
			case OP_ZEXT_I4:
				values [ins->dreg] = LLVMBuildZExt (builder, lhs, LLVMInt64Type (), dname);
				break;
			case OP_TRUNC_I4:
				values [ins->dreg] = LLVMBuildTrunc (builder, lhs, LLVMInt32Type (), dname);
				break;
			case OP_LOCALLOC_IMM: {
				LLVMValueRef v;

				guint32 size = ins->inst_imm;
				size = (size + (MONO_ARCH_FRAME_ALIGNMENT - 1)) & ~ (MONO_ARCH_FRAME_ALIGNMENT - 1);

				v = mono_llvm_build_alloca (builder, LLVMInt8Type (), LLVMConstInt (LLVMInt32Type (), size, FALSE), MONO_ARCH_FRAME_ALIGNMENT, "");

				if (ins->flags & MONO_INST_INIT) {
					LLVMValueRef args [4];

					args [0] = v;
					args [1] = LLVMConstInt (LLVMInt8Type (), 0, FALSE);
					args [2] = LLVMConstInt (LLVMInt32Type (), size, FALSE);
					args [3] = LLVMConstInt (LLVMInt32Type (), MONO_ARCH_FRAME_ALIGNMENT, FALSE);
					LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.memset.i32"), args, 4, "");
				}

				values [ins->dreg] = v;
				break;
			}
			case OP_LOCALLOC: {
				LLVMValueRef v, size;
				
				size = LLVMBuildAnd (builder, LLVMBuildAdd (builder, lhs, LLVMConstInt (LLVMInt32Type (), MONO_ARCH_FRAME_ALIGNMENT - 1, FALSE), ""), LLVMConstInt (LLVMInt32Type (), ~ (MONO_ARCH_FRAME_ALIGNMENT - 1), FALSE), "");

				v = mono_llvm_build_alloca (builder, LLVMInt8Type (), size, MONO_ARCH_FRAME_ALIGNMENT, "");

				if (ins->flags & MONO_INST_INIT) {
					LLVMValueRef args [4];

					args [0] = v;
					args [1] = LLVMConstInt (LLVMInt8Type (), 0, FALSE);
					args [2] = size;
					args [3] = LLVMConstInt (LLVMInt32Type (), MONO_ARCH_FRAME_ALIGNMENT, FALSE);
					LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.memset.i32"), args, 4, "");
				}
				values [ins->dreg] = v;
				break;
			}

			case OP_LOADI1_MEMBASE:
			case OP_LOADU1_MEMBASE:
			case OP_LOADI2_MEMBASE:
			case OP_LOADU2_MEMBASE:
			case OP_LOADI4_MEMBASE:
			case OP_LOADU4_MEMBASE:
			case OP_LOADI8_MEMBASE:
			case OP_LOADR4_MEMBASE:
			case OP_LOADR8_MEMBASE:
			case OP_LOAD_MEMBASE:
			case OP_LOADI8_MEM:
			case OP_LOADU1_MEM:
			case OP_LOADU2_MEM:
			case OP_LOADI4_MEM:
			case OP_LOADU4_MEM:
			case OP_LOAD_MEM: {
				int size = 8;
				LLVMValueRef index;
				LLVMTypeRef t;
				gboolean sext = FALSE, zext = FALSE;

				t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

				if (sext || zext)
					dname = (char*)"";

				/* 
				 * We emit volatile loads because otherwise LLVM will
				 * generate invalid code when encountering a load from a
				 * NULL address.
				 * FIXME: Avoid this somehow.
				 */
				g_assert (ins->inst_offset % size == 0);
				if ((ins->opcode == OP_LOADI8_MEM) || (ins->opcode == OP_LOAD_MEM) || (ins->opcode == OP_LOADI4_MEM) || (ins->opcode == OP_LOADU4_MEM) || (ins->opcode == OP_LOADU1_MEM) || (ins->opcode == OP_LOADU2_MEM)) {
					values [ins->dreg] = mono_llvm_build_volatile_load (builder, convert (ctx, LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE), LLVMPointerType (t, 0)), dname);
				} else if (ins->inst_offset == 0) {
					values [ins->dreg] = mono_llvm_build_volatile_load (builder, convert (ctx, values [ins->inst_basereg], LLVMPointerType (t, 0)), dname);
				} else {
					index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);				
					values [ins->dreg] = mono_llvm_build_volatile_load (builder, LLVMBuildGEP (builder, convert (ctx, values [ins->inst_basereg], LLVMPointerType (t, 0)), &index, 1, ""), dname);
				}
				if (sext)
					values [ins->dreg] = LLVMBuildSExt (builder, values [ins->dreg], LLVMInt32Type (), dname);
				else if (zext)
					values [ins->dreg] = LLVMBuildZExt (builder, values [ins->dreg], LLVMInt32Type (), dname);
				else if (ins->opcode == OP_LOADR4_MEMBASE)
					values [ins->dreg] = LLVMBuildFPExt (builder, values [ins->dreg], LLVMDoubleType (), dname);
				break;
			}
				
			case OP_STOREI1_MEMBASE_REG:
			case OP_STOREI2_MEMBASE_REG:
			case OP_STOREI4_MEMBASE_REG:
			case OP_STOREI8_MEMBASE_REG:
			case OP_STORER4_MEMBASE_REG:
			case OP_STORER8_MEMBASE_REG:
			case OP_STORE_MEMBASE_REG: {
				int size = 8;
				LLVMValueRef index;
				LLVMTypeRef t;
				gboolean sext = FALSE, zext = FALSE;

				t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

				g_assert (ins->inst_offset % size == 0);
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);				
				LLVMBuildStore (builder, convert (ctx, values [ins->sreg1], t), LLVMBuildGEP (builder, convert (ctx, values [ins->inst_destbasereg], LLVMPointerType (t, 0)), &index, 1, ""));
				break;
			}

			case OP_STOREI1_MEMBASE_IMM:
			case OP_STOREI2_MEMBASE_IMM:
			case OP_STOREI4_MEMBASE_IMM:
			case OP_STOREI8_MEMBASE_IMM:
			case OP_STORE_MEMBASE_IMM: {
				int size = 8;
				LLVMValueRef index;
				LLVMTypeRef t;
				gboolean sext = FALSE, zext = FALSE;

				t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

				g_assert (ins->inst_offset % size == 0);
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);				
				LLVMBuildStore (builder, convert (ctx, LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE), t), LLVMBuildGEP (builder, convert (ctx, values [ins->inst_destbasereg], LLVMPointerType (t, 0)), &index, 1, ""));
				break;
			}

			case OP_CHECK_THIS:
				mono_llvm_build_volatile_load (builder, convert (ctx, values [ins->sreg1], LLVMPointerType (IntPtrType (), 0)), "");
				break;
			case OP_OUTARG_VTRETADDR:
				break;
			case OP_VOIDCALL:
			case OP_CALL:
			case OP_LCALL:
			case OP_FCALL:
			case OP_VCALL:
			case OP_VOIDCALL_MEMBASE:
			case OP_CALL_MEMBASE:
			case OP_LCALL_MEMBASE:
			case OP_FCALL_MEMBASE:
			case OP_VCALL_MEMBASE:
			case OP_VOIDCALL_REG:
			case OP_CALL_REG:
			case OP_LCALL_REG:
			case OP_FCALL_REG:
			case OP_VCALL_REG: {
				MonoCallInst *call = (MonoCallInst*)ins;
				MonoMethodSignature *sig = call->signature;
				LLVMValueRef callee, lcall;
				LLVMValueRef *args;
				LLVMCallInfo *cinfo;
				GSList *l;
				int i, pindex;
				gboolean vretaddr;
				LLVMTypeRef llvm_sig;
				gpointer target;
				int *pindexes;
				gboolean virtual, calli;

				cinfo = call->cinfo;

				vretaddr = cinfo && cinfo->ret.storage == LLVMArgVtypeRetAddr;

				llvm_sig = sig_to_llvm_sig (ctx, sig, cinfo);
				CHECK_FAILURE (ctx);

				virtual = (ins->opcode == OP_VOIDCALL_MEMBASE || ins->opcode == OP_CALL_MEMBASE || ins->opcode == OP_VCALL_MEMBASE || ins->opcode == OP_LCALL_MEMBASE || ins->opcode == OP_FCALL_MEMBASE);
				calli = (ins->opcode == OP_VOIDCALL_REG || ins->opcode == OP_CALL_REG || ins->opcode == OP_VCALL_REG || ins->opcode == OP_LCALL_REG || ins->opcode == OP_FCALL_REG);

				pindexes = mono_mempool_alloc0 (cfg->mempool, (sig->param_count + 2) * sizeof (guint32));

				/* FIXME: Avoid creating duplicate methods */

				if (ins->flags & MONO_INST_HAS_METHOD) {
					if (virtual) {
						callee = NULL;
					} else {
						if (cfg->compile_aot) {
							callee = get_plt_entry (ctx, llvm_sig, MONO_PATCH_INFO_METHOD, call->method);
							if (!callee)
								LLVM_FAILURE (ctx, "can't encode patch");
						} else {
							callee = LLVMAddFunction (module, "", llvm_sig);
 
							target =
								mono_create_jit_trampoline_in_domain (mono_domain_get (),
																	  call->method);
							LLVMAddGlobalMapping (ee, callee, target);
						}
					}
				} else if (calli) {
				} else {
					MonoJitICallInfo *info = mono_find_jit_icall_by_addr (call->fptr);

					if (info) {
						/*
						MonoJumpInfo ji;

						memset (&ji, 0, sizeof (ji));
						ji.type = MONO_PATCH_INFO_JIT_ICALL_ADDR;
						ji.data.target = info->name;

						target = mono_resolve_patch_target (cfg->method, cfg->domain, NULL, &ji, FALSE);
						*/
						if (cfg->compile_aot) {
							callee = get_plt_entry (ctx, llvm_sig, MONO_PATCH_INFO_INTERNAL_METHOD, (char*)info->name);
							if (!callee)
								LLVM_FAILURE (ctx, "can't encode patch");
						} else {
							callee = LLVMAddFunction (module, "", llvm_sig);
							target = (gpointer)mono_icall_get_wrapper (info);
							LLVMAddGlobalMapping (ee, callee, target);
						}
					} else {
						if (cfg->compile_aot) {
							callee = NULL;
							if (cfg->abs_patches) {
								MonoJumpInfo *abs_ji = g_hash_table_lookup (cfg->abs_patches, call->fptr);
								if (abs_ji) {
									callee = get_plt_entry (ctx, llvm_sig, abs_ji->type, abs_ji->data.target);
									if (!callee)
										LLVM_FAILURE (ctx, "can't encode patch");
								}
 							}
							if (!callee)
								LLVM_FAILURE (ctx, "aot");
						} else {
							callee = LLVMAddFunction (module, "", llvm_sig);
							target = NULL;
							if (cfg->abs_patches) {
								MonoJumpInfo *abs_ji = g_hash_table_lookup (cfg->abs_patches, call->fptr);
								if (abs_ji) {
									/*
									 * The monitor entry/exit trampolines might have
									 * their own calling convention on some platforms.
									 */
#ifndef TARGET_AMD64
									if (abs_ji->type == MONO_PATCH_INFO_MONITOR_ENTER || abs_ji->type == MONO_PATCH_INFO_MONITOR_EXIT)
										LLVM_FAILURE (ctx, "monitor enter/exit");
#endif
									target = mono_resolve_patch_target (cfg->method, cfg->domain, NULL, abs_ji, FALSE);
									LLVMAddGlobalMapping (ee, callee, target);
								}
							}
							if (!target)
								LLVMAddGlobalMapping (ee, callee, (gpointer)call->fptr);
						}
					}
				}

				if (virtual) {
					int size = sizeof (gpointer);
					LLVMValueRef index;

					g_assert (ins->inst_offset % size == 0);
					index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);				

					// FIXME: mono_arch_get_vcall_slot () can't decode the code
					// generated by LLVM
					//LLVM_FAILURE (ctx, "virtual call");

					if (call->method && call->method->klass->flags & TYPE_ATTRIBUTE_INTERFACE) {
#ifdef MONO_ARCH_HAVE_LLVM_IMT_TRAMPOLINE
						if (cfg->compile_aot) {
							MonoJumpInfoImtTramp *imt_tramp = g_new0 (MonoJumpInfoImtTramp, 1);
							imt_tramp->method = call->method;
							imt_tramp->vt_offset = call->inst.inst_offset;

							callee = get_plt_entry (ctx, llvm_sig, MONO_PATCH_INFO_LLVM_IMT_TRAMPOLINE, imt_tramp);
						} else {
							callee = LLVMAddFunction (module, "", llvm_sig);
							target = mono_create_llvm_imt_trampoline (cfg->domain, call->method, call->inst.inst_offset);
							LLVMAddGlobalMapping (ee, callee, target);
						}
#else
						/* No support for passing the IMT argument */
						LLVM_FAILURE (ctx, "imt");
#endif
					} else {
						callee = convert (ctx, LLVMBuildLoad (builder, LLVMBuildGEP (builder, convert (ctx, values [ins->inst_basereg], LLVMPointerType (LLVMPointerType (IntPtrType (), 0), 0)), &index, 1, ""), ""), LLVMPointerType (llvm_sig, 0));
					}
				} else if (calli) {
					callee = convert (ctx, values [ins->sreg1], LLVMPointerType (llvm_sig, 0));
				} else {
					if (ins->flags & MONO_INST_HAS_METHOD) {
					}
				}

				/* 
				 * Collect and convert arguments
				 */

				args = alloca (sizeof (LLVMValueRef) * ((sig->param_count * 2) + sig->hasthis + vretaddr));
				l = call->out_ireg_args;
				pindex = 0;
				if (vretaddr) {
					if (!addresses [call->inst.dreg])
						addresses [call->inst.dreg] = build_alloca (ctx, sig->ret);
					args [pindex ++] = LLVMBuildPtrToInt (builder, addresses [call->inst.dreg], IntPtrType (), "");
				}

				for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
					guint32 regpair;
					int reg;
					LLVMArgInfo *ainfo = call->cinfo ? &call->cinfo->args [i] : NULL;

					regpair = (guint32)(gssize)(l->data);
					reg = regpair & 0xffffff;
					args [pindex] = values [reg];
					if (ainfo->storage == LLVMArgVtypeInReg) {
						int j;
						LLVMValueRef regs [2];
						guint32 nregs;

						g_assert (ainfo);

						g_assert (addresses [reg]);

						emit_vtype_to_reg (ctx, builder, sig->params [i - sig->hasthis], addresses [reg], ainfo, regs, &nregs);
						for (j = 0; j < nregs; ++j)
							args [pindex ++] = regs [j];

						// FIXME: alignment
						// FIXME: Get rid of the VMOVE
					} else if (ainfo->storage == LLVMArgVtypeByVal) {
						g_assert (addresses [reg]);
						args [pindex] = addresses [reg];
						pindexes [i] = pindex;
						pindex ++;
					} else {
						g_assert (args [pindex]);
						if (i == 0 && sig->hasthis)
							args [pindex] = convert (ctx, args [pindex], IntPtrType ());
						else
							args [pindex] = convert (ctx, args [pindex], type_to_llvm_arg_type (ctx, sig->params [i - sig->hasthis]));
						pindex ++;
					}

					l = l->next;
				}

				// FIXME: Align call sites

				/*
				 * Emit the call
				 */

				lcall = emit_call (ctx, bb, &builder, callee, args, pindex);

				/* Add byval attributes if needed */
				for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
					LLVMArgInfo *ainfo = call->cinfo ? &call->cinfo->args [i] : NULL;

					if (ainfo && ainfo->storage == LLVMArgVtypeByVal) {
						LLVMAddInstrAttribute (lcall, pindexes [i] + 1, LLVMByValAttribute);
					}
				}

				/*
				 * Convert the result
				 */
				if (cinfo && cinfo->ret.storage == LLVMArgVtypeInReg) {
					LLVMValueRef regs [2];

					if (!addresses [ins->dreg])
						addresses [ins->dreg] = build_alloca (ctx, sig->ret);

					regs [0] = LLVMBuildExtractValue (builder, lcall, 0, "");
					if (cinfo->ret.pair_storage [1] != LLVMArgNone)
						regs [1] = LLVMBuildExtractValue (builder, lcall, 1, "");
					
					emit_reg_to_vtype (ctx, builder, sig->ret, addresses [ins->dreg], &cinfo->ret, regs);
				} else if (sig->ret->type != MONO_TYPE_VOID && !vretaddr) {
					/* If the method returns an unsigned value, need to zext it */

					values [ins->dreg] = convert_full (ctx, lcall, llvm_type_to_stack_type (type_to_llvm_type (ctx, sig->ret)), type_is_unsigned (ctx, sig->ret));
				}
				break;
			}
			case OP_AOTCONST: {
				guint32 got_offset;
 				LLVMValueRef indexes [2];
				MonoJumpInfo *ji;

				/* 
				 * FIXME: Can't allocate from the cfg mempool since that is freed if
				 * the LLVM compile fails.
				 */
				ji = g_new0 (MonoJumpInfo, 1);
				ji->type = (MonoJumpInfoType)ins->inst_i1;
				ji->data.target = ins->inst_p0;

				ji = mono_aot_patch_info_dup (ji);

				ji->next = cfg->patch_info;
				cfg->patch_info = ji;
				   
				//mono_add_patch_info (cfg, 0, (MonoJumpInfoType)ins->inst_i1, ins->inst_p0);
				got_offset = mono_aot_get_got_offset (cfg->patch_info);
 
 				indexes [0] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
				indexes [1] = LLVMConstInt (LLVMInt32Type (), (gssize)got_offset, FALSE);
 				values [ins->dreg] = LLVMBuildLoad (builder, LLVMBuildGEP (builder, ctx->lmodule->got_var, indexes, 2, ""), dname);
 				break;
 			}
			case OP_NOT_REACHED:
				LLVMBuildUnreachable (builder);
				has_terminator = TRUE;
				g_assert (bb->block_num < cfg->max_block_num);
				unreachable [bb->block_num] = TRUE;
				/* Might have instructions after this */
				while (ins->next) {
					MonoInst *next = ins->next;
					MONO_DELETE_INS (bb, next);
				}				
				break;
			case OP_LDADDR: {
				MonoInst *var = ins->inst_p0;

				values [ins->dreg] = addresses [var->dreg];
				break;
			}
			case OP_SIN: {
				LLVMValueRef args [1];

				args [0] = convert (ctx, lhs, LLVMDoubleType ());
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.sin.f64"), args, 1, dname);
				break;
			}
			case OP_COS: {
				LLVMValueRef args [1];

				args [0] = convert (ctx, lhs, LLVMDoubleType ());
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.cos.f64"), args, 1, dname);
				break;
			}
				/* test_0_sqrt_nan fails with LLVM */
				/*
			case OP_SQRT: {
				LLVMValueRef args [1];

				args [0] = lhs;
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.sqrt.f64"), args, 1, dname);
				break;
			}
				*/

			case OP_ABS: {
				LLVMValueRef args [1];

				args [0] = lhs;
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, "fabs"), args, 1, dname);
				break;
			}

			case OP_IMIN:
			case OP_LMIN: {
				LLVMValueRef v = LLVMBuildICmp (builder, LLVMIntSLE, lhs, rhs, "");
				values [ins->dreg] = LLVMBuildSelect (builder, v, lhs, rhs, dname);
				break;
			}
			case OP_IMAX:
			case OP_LMAX: {
				LLVMValueRef v = LLVMBuildICmp (builder, LLVMIntSGE, lhs, rhs, "");
				values [ins->dreg] = LLVMBuildSelect (builder, v, lhs, rhs, dname);
				break;
			}
			case OP_IMIN_UN:
			case OP_LMIN_UN: {
				LLVMValueRef v = LLVMBuildICmp (builder, LLVMIntULE, lhs, rhs, "");
				values [ins->dreg] = LLVMBuildSelect (builder, v, lhs, rhs, dname);
				break;
			}
			case OP_IMAX_UN:
			case OP_LMAX_UN: {
				LLVMValueRef v = LLVMBuildICmp (builder, LLVMIntUGE, lhs, rhs, "");
				values [ins->dreg] = LLVMBuildSelect (builder, v, lhs, rhs, dname);
				break;
			}
			case OP_ATOMIC_EXCHANGE_I4: {
				LLVMValueRef args [2];

				g_assert (ins->inst_offset == 0);

				args [0] = convert (ctx, lhs, LLVMPointerType (LLVMInt32Type (), 0));
				args [1] = rhs;
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.atomic.swap.i32.p0i32"), args, 2, dname);
				break;
			}
			case OP_ATOMIC_EXCHANGE_I8: {
				LLVMValueRef args [2];

				g_assert (ins->inst_offset == 0);

				args [0] = convert (ctx, lhs, LLVMPointerType (LLVMInt64Type (), 0));
				args [1] = rhs;
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.atomic.swap.i64.p0i64"), args, 2, dname);
				break;
			}
			case OP_ATOMIC_ADD_NEW_I4: {
				LLVMValueRef args [2];

				g_assert (ins->inst_offset == 0);

				args [0] = convert (ctx, lhs, LLVMPointerType (LLVMInt32Type (), 0));
				args [1] = rhs;
				values [ins->dreg] = LLVMBuildAdd (builder, LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.atomic.load.add.i32.p0i32"), args, 2, ""), args [1], dname);
				break;
			}
			case OP_ATOMIC_ADD_NEW_I8: {
				LLVMValueRef args [2];

				g_assert (ins->inst_offset == 0);

				args [0] = convert (ctx, lhs, LLVMPointerType (LLVMInt64Type (), 0));
				args [1] = convert (ctx, rhs, LLVMInt64Type ());
				values [ins->dreg] = LLVMBuildAdd (builder, LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.atomic.load.add.i64.p0i64"), args, 2, ""), args [1], dname);
				break;
			}
			case OP_ATOMIC_CAS_I4:
			case OP_ATOMIC_CAS_I8: {
				LLVMValueRef args [3];
				LLVMTypeRef t;
				const char *intrins;
				
				if (ins->opcode == OP_ATOMIC_CAS_I4) {
					t = LLVMInt32Type ();
					intrins = "llvm.atomic.cmp.swap.i32.p0i32";
				} else {
					t = LLVMInt64Type ();
					intrins = "llvm.atomic.cmp.swap.i64.p0i64";
				}

				args [0] = convert (ctx, lhs, LLVMPointerType (t, 0));
				/* comparand */
				args [1] = convert (ctx, values [ins->sreg3], t);
				/* new value */
				args [2] = convert (ctx, values [ins->sreg2], t);
				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, intrins), args, 3, dname);
				break;
			}
			case OP_MEMORY_BARRIER: {
				LLVMValueRef args [5];

				for (i = 0; i < 5; ++i)
					args [i] = LLVMConstInt (LLVMInt1Type (), TRUE, TRUE);

				LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.memory.barrier"), args, 5, "");
				break;
			}
			case OP_RELAXED_NOP: {
#if defined(TARGET_AMD64) || defined(TARGET_X86)
				/* No way to get LLVM to emit this */
				LLVM_FAILURE (ctx, "relaxed_nop");
#else
				break;
#endif
			}
			case OP_TLS_GET: {
#if defined(TARGET_AMD64) || defined(TARGET_X86)
#ifdef TARGET_AMD64
				// 257 == FS segment register
				LLVMTypeRef ptrtype = LLVMPointerType (IntPtrType (), 257);
#else
				// 256 == GS segment register
				LLVMTypeRef ptrtype = LLVMPointerType (IntPtrType (), 256);
#endif

				// FIXME: XEN
				values [ins->dreg] = LLVMBuildLoad (builder, LLVMBuildIntToPtr (builder, LLVMConstInt (IntPtrType (), ins->inst_offset, TRUE), ptrtype, ""), "");
#else
				LLVM_FAILURE (ctx, "opcode tls-get");
#endif

				break;
			}

			/*
			 * Overflow opcodes.
			 */
			case OP_IADD_OVF:
			case OP_IADD_OVF_UN:
			case OP_ISUB_OVF:
			case OP_ISUB_OVF_UN:
			case OP_IMUL_OVF:
			case OP_IMUL_OVF_UN:
#if SIZEOF_VOID_P == 8
			case OP_LADD_OVF:
			case OP_LADD_OVF_UN:
			case OP_LSUB_OVF:
			case OP_LSUB_OVF_UN:
			case OP_LMUL_OVF:
			case OP_LMUL_OVF_UN:
#endif
			{
				LLVMValueRef args [2], val, ovf, func;

				emit_cond_throw_pos (ctx);

				args [0] = convert (ctx, lhs, op_to_llvm_type (ins->opcode));
				args [1] = convert (ctx, rhs, op_to_llvm_type (ins->opcode));
				func = LLVMGetNamedFunction (module, ovf_op_to_intrins (ins->opcode));
				g_assert (func);
				val = LLVMBuildCall (builder, func, args, 2, "");
				values [ins->dreg] = LLVMBuildExtractValue (builder, val, 0, dname);
				ovf = LLVMBuildExtractValue (builder, val, 1, "");
				emit_cond_system_exception (ctx, bb, "OverflowException", ovf);
				builder = ctx->builder;
				break;
			}

			/* 
			 * Valuetypes.
			 *   We currently model them using arrays. Promotion to local vregs is 
			 * disabled for them in mono_handle_global_vregs () in the LLVM case, 
			 * so we always have an entry in cfg->varinfo for them.
			 * FIXME: Is this needed ?
			 */
			case OP_VZERO: {
				MonoClass *klass = ins->klass;
				LLVMValueRef args [4];

				if (!klass) {
					// FIXME:
					LLVM_FAILURE (ctx, "!klass");
					break;
				}

				if (!addresses [ins->dreg])
					addresses [ins->dreg] = build_alloca (ctx, &klass->byval_arg);
				args [0] = LLVMBuildBitCast (builder, addresses [ins->dreg], LLVMPointerType (LLVMInt8Type (), 0), "");
				args [1] = LLVMConstInt (LLVMInt8Type (), 0, FALSE);
				args [2] = LLVMConstInt (LLVMInt32Type (), mono_class_value_size (klass, NULL), FALSE);
				// FIXME: Alignment
				args [3] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
				LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.memset.i32"), args, 4, "");
				break;
			}

			case OP_STOREV_MEMBASE:
			case OP_LOADV_MEMBASE:
			case OP_VMOVE: {
				MonoClass *klass = ins->klass;
				LLVMValueRef src, dst, args [4];
				gboolean done = FALSE;

				if (!klass) {
					// FIXME:
					LLVM_FAILURE (ctx, "!klass");
					break;
				}

				switch (ins->opcode) {
				case OP_STOREV_MEMBASE:
					if (!addresses [ins->sreg1]) {
						/* SIMD */
						g_assert (values [ins->sreg1]);
						dst = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_destbasereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (type_to_llvm_type (ctx, &klass->byval_arg), 0));
						LLVMBuildStore (builder, values [ins->sreg1], dst);
						done = TRUE;
					} else {
						src = LLVMBuildBitCast (builder, addresses [ins->sreg1], LLVMPointerType (LLVMInt8Type (), 0), "");
						dst = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_destbasereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (LLVMInt8Type (), 0));
					}
					break;
				case OP_LOADV_MEMBASE:
					if (!addresses [ins->dreg])
						addresses [ins->dreg] = build_alloca (ctx, &klass->byval_arg);
					src = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_basereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (LLVMInt8Type (), 0));
					dst = LLVMBuildBitCast (builder, addresses [ins->dreg], LLVMPointerType (LLVMInt8Type (), 0), "");
					break;
				case OP_VMOVE:
					if (!addresses [ins->sreg1])
						addresses [ins->sreg1] = build_alloca (ctx, &klass->byval_arg);
					if (!addresses [ins->dreg])
						addresses [ins->dreg] = build_alloca (ctx, &klass->byval_arg);
					src = LLVMBuildBitCast (builder, addresses [ins->sreg1], LLVMPointerType (LLVMInt8Type (), 0), "");
					dst = LLVMBuildBitCast (builder, addresses [ins->dreg], LLVMPointerType (LLVMInt8Type (), 0), "");
					break;
				default:
					g_assert_not_reached ();
				}

				if (done)
					break;

				args [0] = dst;
				args [1] = src;
				args [2] = LLVMConstInt (LLVMInt32Type (), mono_class_value_size (klass, NULL), FALSE);
				args [3] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
				// FIXME: Alignment
				args [3] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
				LLVMBuildCall (builder, LLVMGetNamedFunction (module, "llvm.memcpy.i32"), args, 4, "");
				break;
			}
			case OP_LLVM_OUTARG_VT:
				if (!addresses [ins->sreg1]) {
					addresses [ins->sreg1] = build_alloca (ctx, &ins->klass->byval_arg);
					g_assert (values [ins->sreg1]);
					LLVMBuildStore (builder, values [ins->sreg1], addresses [ins->sreg1]);
				}
				addresses [ins->dreg] = addresses [ins->sreg1];
				break;

			/* 
			 * SIMD
			 */
			case OP_XZERO: {
				values [ins->dreg] = LLVMConstNull (type_to_llvm_type (ctx, &ins->klass->byval_arg));
				break;
			}
			case OP_LOADX_MEMBASE: {
				LLVMTypeRef t = type_to_llvm_type (ctx, &ins->klass->byval_arg);
				LLVMValueRef src;

				src = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_basereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (t, 0));
				values [ins->dreg] = LLVMBuildLoad (builder, src, "");
				break;
			}
			case OP_ADDPD:
			case OP_ADDPS:
			case OP_PADDB:
			case OP_PADDW:
			case OP_PADDD:
			case OP_PADDQ:
				values [ins->dreg] = LLVMBuildAdd (builder, lhs, rhs, "");
				break;
			case OP_SUBPD:
			case OP_SUBPS:
			case OP_PSUBB:
			case OP_PSUBW:
			case OP_PSUBD:
			case OP_PSUBQ:
				values [ins->dreg] = LLVMBuildSub (builder, lhs, rhs, "");
				break;
			case OP_MULPD:
			case OP_MULPS:
				values [ins->dreg] = LLVMBuildMul (builder, lhs, rhs, "");
				break;
			case OP_DIVPD:
			case OP_DIVPS:
				values [ins->dreg] = LLVMBuildFDiv (builder, lhs, rhs, "");
				break;
			case OP_PAND:
				values [ins->dreg] = LLVMBuildAnd (builder, lhs, rhs, "");
				break;
			case OP_POR:
				values [ins->dreg] = LLVMBuildOr (builder, lhs, rhs, "");
				break;
			case OP_PXOR:
				values [ins->dreg] = LLVMBuildXor (builder, lhs, rhs, "");
				break;
			case OP_ANDPS:
			case OP_ANDNPS:
			case OP_ORPS:
			case OP_XORPS:
			case OP_ANDPD:
			case OP_ANDNPD:
			case OP_ORPD:
			case OP_XORPD: {
				LLVMTypeRef t, rt;
				LLVMValueRef v;

				switch (ins->opcode) {
				case OP_ANDPS:
				case OP_ANDNPS:
				case OP_ORPS:
				case OP_XORPS:
					t = LLVMVectorType (LLVMInt32Type (), 4);
					rt = LLVMVectorType (LLVMFloatType (), 4);
					break;
				case OP_ANDPD:
				case OP_ANDNPD:
				case OP_ORPD:
				case OP_XORPD:
					t = LLVMVectorType (LLVMInt64Type (), 2);
					rt = LLVMVectorType (LLVMDoubleType (), 2);
					break;
				default:
					t = LLVMInt32Type ();
					rt = LLVMInt32Type ();
					g_assert_not_reached ();
				}

				lhs = LLVMBuildBitCast (builder, lhs, t, "");
				rhs = LLVMBuildBitCast (builder, rhs, t, "");
				switch (ins->opcode) {
				case OP_ANDPS:
				case OP_ANDPD:
					v = LLVMBuildAnd (builder, lhs, rhs, "");
					break;
				case OP_ORPS:
				case OP_ORPD:
					v = LLVMBuildOr (builder, lhs, rhs, "");
					break;
				case OP_XORPS:
				case OP_XORPD:
					v = LLVMBuildXor (builder, lhs, rhs, "");
					break;
				case OP_ANDNPS:
				case OP_ANDNPD:
					v = LLVMBuildAnd (builder, lhs, LLVMBuildNot (builder, rhs, ""), "");
					break;
				}
				values [ins->dreg] = LLVMBuildBitCast (builder, v, rt, "");
				break;
			}
			case OP_MINPD:
			case OP_MINPS:
			case OP_MAXPD:
			case OP_MAXPS:
			case OP_PMIND_UN:
			case OP_PMINW_UN:
			case OP_PMINB_UN:
			case OP_PMAXD_UN:
			case OP_PMAXW_UN:
			case OP_PMAXB_UN: {
				LLVMValueRef args [2];

				args [0] = lhs;
				args [1] = rhs;

				values [ins->dreg] = LLVMBuildCall (builder, LLVMGetNamedFunction (module, simd_op_to_intrins (ins->opcode)), args, 2, dname);
				break;
			}
			case OP_EXTRACT_R8:
			case OP_EXTRACT_I8:
			case OP_EXTRACT_I4:
			case OP_EXTRACT_I2:
			case OP_EXTRACT_U2:
			case OP_EXTRACT_I1:
			case OP_EXTRACT_U1: {
				LLVMTypeRef t;

				switch (ins->opcode) {
				case OP_EXTRACT_R8:
					t = LLVMVectorType (LLVMDoubleType (), 2);
					break;
				case OP_EXTRACT_I8:
					t = LLVMVectorType (LLVMInt64Type (), 2);
					break;
				case OP_EXTRACT_I4:
					t = LLVMVectorType (LLVMInt32Type (), 4);
					break;
				case OP_EXTRACT_I2:
				case OP_EXTRACT_U2:
					t = LLVMVectorType (LLVMInt16Type (), 8);
					break;
				case OP_EXTRACT_I1:
				case OP_EXTRACT_U1:
					t = LLVMVectorType (LLVMInt8Type (), 16);
					break;
				default:
					t = LLVMInt32Type ();
					g_assert_not_reached ();
				}

				lhs = LLVMBuildBitCast (builder, lhs, t, "");
				values [ins->dreg] = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), "");
				break;
			}

			case OP_DUMMY_USE:
				break;

			/*
			 * EXCEPTION HANDLING
			 */
			case OP_IMPLICIT_EXCEPTION:
				/* This marks a place where an implicit exception can happen */
				if (bb->region != -1)
					LLVM_FAILURE (ctx, "implicit-exception");
				break;
			case OP_THROW: {
				MonoMethodSignature *throw_sig;
				LLVMValueRef callee, arg;

				if (!ctx->lmodule->throw) {
					throw_sig = mono_metadata_signature_alloc (mono_defaults.corlib, 1);
					throw_sig->ret = &mono_defaults.void_class->byval_arg;
					throw_sig->params [0] = &mono_defaults.object_class->byval_arg;
					if (cfg->compile_aot) {
						callee = get_plt_entry (ctx, sig_to_llvm_sig (ctx, throw_sig, NULL), MONO_PATCH_INFO_INTERNAL_METHOD, "mono_arch_throw_exception");
					} else {
						callee = LLVMAddFunction (module, "mono_arch_throw_exception", sig_to_llvm_sig (ctx, throw_sig, NULL));
						LLVMAddGlobalMapping (ee, callee, resolve_patch (cfg, MONO_PATCH_INFO_INTERNAL_METHOD, "mono_arch_throw_exception"));
					}

					mono_memory_barrier ();
					ctx->lmodule->throw = callee;
				}
				arg = convert (ctx, values [ins->sreg1], type_to_llvm_type (ctx, &mono_defaults.object_class->byval_arg));
				emit_call (ctx, bb, &builder, ctx->lmodule->throw, &arg, 1);
				break;
			}
			case OP_CALL_HANDLER: {
				/* 
				 * We don't 'call' handlers, but instead simply branch to them.
				 * The code generated by ENDFINALLY will branch back to us.
				 */
				LLVMBasicBlockRef finally_bb, noex_bb;
				GSList *bb_list;

				finally_bb = get_bb (ctx, ins->inst_target_bb);

				bb_list = bblocks [ins->inst_target_bb->block_num].call_handler_return_bbs;

				/* 
				 * Set the indicator variable for the finally clause.
				 */
				lhs = bblocks [ins->inst_target_bb->block_num].finally_ind;
				g_assert (lhs);
				LLVMBuildStore (builder, LLVMConstInt (LLVMInt32Type (), g_slist_length (bb_list) + 1, FALSE), lhs);
				
				/* Branch to the finally clause */
				LLVMBuildBr (builder, finally_bb);

				noex_bb = gen_bb (ctx, "CALL_HANDLER_CONT_BB");
				// FIXME: Use a mempool
				bblocks [ins->inst_target_bb->block_num].call_handler_return_bbs = g_slist_append (bblocks [ins->inst_target_bb->block_num].call_handler_return_bbs, noex_bb);

				builder = ctx->builder = create_builder (ctx);
				LLVMPositionBuilderAtEnd (ctx->builder, noex_bb);

				bblocks [bb->block_num].end_bblock = noex_bb;
				break;
			}
			case OP_START_HANDLER: {
				break;
			}
			case OP_ENDFINALLY: {
				LLVMBasicBlockRef resume_bb;
				MonoBasicBlock *handler_bb;
				LLVMValueRef val, switch_ins;
				GSList *bb_list;

				handler_bb = g_hash_table_lookup (ctx->region_to_handler, GUINT_TO_POINTER (mono_get_block_region_notry (cfg, bb->region)));
				g_assert (handler_bb);
				lhs = bblocks [handler_bb->block_num].finally_ind;
				g_assert (lhs);

				bb_list = bblocks [handler_bb->block_num].call_handler_return_bbs;

				resume_bb = gen_bb (ctx, "ENDFINALLY_RESUME_BB");

				/* Load the finally variable */
				val = LLVMBuildLoad (builder, lhs, "");

				/* Reset the variable */
				LLVMBuildStore (builder, LLVMConstInt (LLVMInt32Type (), 0, FALSE), lhs);

				/* Branch to either resume_bb, or to the bblocks in bb_list */
				switch_ins = LLVMBuildSwitch (builder, val, resume_bb, g_slist_length (bb_list));
				/* 
				 * The other targets are added at the end to handle OP_CALL_HANDLER
				 * opcodes processed later.
				 */
				bblocks [handler_bb->block_num].endfinally_switch = switch_ins;
				/*
				for (i = 0; i < g_slist_length (bb_list); ++i)
					LLVMAddCase (switch_ins, LLVMConstInt (LLVMInt32Type (), i + 1, FALSE), g_slist_nth (bb_list, i)->data);
				*/

				builder = ctx->builder = create_builder (ctx);
				LLVMPositionBuilderAtEnd (ctx->builder, resume_bb);

				LLVMBuildCall (builder, LLVMGetNamedFunction (module, "mono_resume_unwind"), NULL, 0, "");
				LLVMBuildUnreachable (builder);
				has_terminator = TRUE;
				break;
			}
			default: {
				char reason [128];

				sprintf (reason, "opcode %s", mono_inst_name (ins->opcode));
				LLVM_FAILURE (ctx, reason);
				break;
			}
			}

			/* Convert the value to the type required by phi nodes */
			if (spec [MONO_INST_DEST] != ' ' && !MONO_IS_STORE_MEMBASE (ins) && vreg_types [ins->dreg]) {
				if (!values [ins->dreg])
					/* vtypes */
					values [ins->dreg] = addresses [ins->dreg];
				else
					values [ins->dreg] = convert (ctx, values [ins->dreg], vreg_types [ins->dreg]);
			}

			/* Add stores for volatile variables */
			if (spec [MONO_INST_DEST] != ' ' && spec [MONO_INST_DEST] != 'v' && !MONO_IS_STORE_MEMBASE (ins))
				emit_volatile_store (ctx, ins->dreg);
		}

		if (!has_terminator && bb->next_bb && (bb == cfg->bb_entry || bb->in_count > 0))
			LLVMBuildBr (builder, get_bb (ctx, bb->next_bb));

		if (bb == cfg->bb_exit && sig->ret->type == MONO_TYPE_VOID)
			LLVMBuildRetVoid (builder);

		if (bb == cfg->bb_entry)
			ctx->last_alloca = LLVMGetLastInstruction (get_bb (ctx, cfg->bb_entry));
	}

	/* Add incoming phi values */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		GSList *l, *ins_list;

		ins_list = bblocks [bb->block_num].phi_nodes;

		for (l = ins_list; l; l = l->next) {
			PhiNode *node = l->data;
			MonoInst *phi = node->phi;
			int sreg1 = node->sreg;
			LLVMBasicBlockRef in_bb;

			if (sreg1 == -1)
				continue;

			in_bb = get_end_bb (ctx, node->in_bb);

			if (unreachable [node->in_bb->block_num])
				continue;

			g_assert (values [sreg1]);

			g_assert (LLVMTypeOf (values [sreg1]) == LLVMTypeOf (values [phi->dreg]));
			LLVMAddIncoming (values [phi->dreg], &values [sreg1], &in_bb, 1);
		}
	}

	/* Create the SWITCH statements for ENDFINALLY instructions */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (bblocks [bb->block_num].endfinally_switch) {
			LLVMValueRef switch_ins = bblocks [bb->block_num].endfinally_switch;
			GSList *bb_list = bblocks [bb->block_num].call_handler_return_bbs;

			for (i = 0; i < g_slist_length (bb_list); ++i)
				LLVMAddCase (switch_ins, LLVMConstInt (LLVMInt32Type (), i + 1, FALSE), g_slist_nth (bb_list, i)->data);
		}
	}

	if (cfg->verbose_level > 1)
		mono_llvm_dump_value (method);

	mark_as_used (module, method);

	if (cfg->compile_aot) {
		/* Don't generate native code, keep the LLVM IR */

		/* Can't delete the method if it has an alias, so only add it if successful */
		if (debug_name) {
			debug_alias = LLVMAddAlias (module, LLVMTypeOf (method), method, debug_name);
			LLVMSetVisibility (debug_alias, LLVMHiddenVisibility);
		}

		if (cfg->compile_aot && cfg->verbose_level)
			printf ("%s emitted as %s\n", mono_method_full_name (cfg->method, TRUE), method_name);

		//LLVMVerifyFunction(method, 0);
	} else {
		mono_llvm_optimize_method (method);

		if (cfg->verbose_level > 1)
			mono_llvm_dump_value (method);

		cfg->native_code = LLVMGetPointerToGlobal (ee, method);

		/* Set by emit_cb */
		g_assert (cfg->code_len);

		/* FIXME: Free the LLVM IL for the function */
	}

	goto CLEANUP;

 FAILURE:

	if (method) {
		/* Need to add unused phi nodes as they can be referenced by other values */
		LLVMBasicBlockRef phi_bb = LLVMAppendBasicBlock (method, "PHI_BB");
		LLVMBuilderRef builder;

		builder = create_builder (ctx);
		LLVMPositionBuilderAtEnd (builder, phi_bb);

		for (i = 0; i < phi_values->len; ++i) {
			LLVMValueRef v = g_ptr_array_index (phi_values, i);
			if (LLVMGetInstructionParent (v) == NULL)
				LLVMInsertIntoBuilder (builder, v);
		}
		
		LLVMDeleteFunction (method);
	}

 CLEANUP:
	g_free (values);
	g_free (addresses);
	g_free (vreg_types);
	g_free (vreg_cli_types);
	g_free (pindexes);
	g_free (debug_name);
	g_ptr_array_free (phi_values, TRUE);
	g_free (ctx->bblocks);
	g_hash_table_destroy (ctx->region_to_handler);
	g_free (method_name);
	g_ptr_array_free (bblock_list, TRUE);

	for (l = ctx->builders; l; l = l->next) {
		LLVMBuilderRef builder = l->data;
		LLVMDisposeBuilder (builder);
	}

	g_free (ctx);

	TlsSetValue (current_cfg_tls_id, NULL);

	mono_loader_unlock ();
}

/*
 * mono_llvm_emit_call:
 *
 *   Same as mono_arch_emit_call () for LLVM.
 */
void
mono_llvm_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	MonoInst *in;
	MonoMethodSignature *sig;
	int i, n, stack_size;
	LLVMArgInfo *ainfo;

	stack_size = 0;

	sig = call->signature;
	n = sig->param_count + sig->hasthis;

	call->cinfo = mono_arch_get_llvm_call_info (cfg, sig);

	if (cfg->disable_llvm)
		return;

	if (sig->call_convention == MONO_CALL_VARARG) {
		cfg->exception_message = g_strdup ("varargs");
		cfg->disable_llvm = TRUE;
	}

	for (i = 0; i < n; ++i) {
		MonoInst *ins;

		ainfo = call->cinfo->args + i;

		in = call->args [i];
			
		/* Simply remember the arguments */
		switch (ainfo->storage) {
		case LLVMArgInIReg:
			MONO_INST_NEW (cfg, ins, OP_MOVE);
			ins->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = in->dreg;
			break;
		case LLVMArgInFPReg:
			MONO_INST_NEW (cfg, ins, OP_FMOVE);
			ins->dreg = mono_alloc_freg (cfg);
			ins->sreg1 = in->dreg;
			break;
		case LLVMArgVtypeByVal:
		case LLVMArgVtypeInReg:
			MONO_INST_NEW (cfg, ins, OP_LLVM_OUTARG_VT);
			ins->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = in->dreg;
			ins->klass = mono_class_from_mono_type (sig->params [i - sig->hasthis]);
			break;
		default:
			call->cinfo = mono_arch_get_llvm_call_info (cfg, sig);
			cfg->exception_message = g_strdup ("ainfo->storage");
			cfg->disable_llvm = TRUE;
			return;
		}

		if (!cfg->disable_llvm) {
			MONO_ADD_INS (cfg->cbb, ins);
			mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, 0, FALSE);
		}
	}
}

static unsigned char*
alloc_cb (LLVMValueRef function, int size)
{
	MonoCompile *cfg;

	cfg = TlsGetValue (current_cfg_tls_id);

	if (cfg) {
		// FIXME: dynamic
		return mono_domain_code_reserve (cfg->domain, size);
	} else {
		return mono_domain_code_reserve (mono_domain_get (), size);
	}
}

static void
emitted_cb (LLVMValueRef function, void *start, void *end)
{
	MonoCompile *cfg;

	cfg = TlsGetValue (current_cfg_tls_id);
	g_assert (cfg);
	cfg->code_len = (guint8*)end - (guint8*)start;
}

static void
exception_cb (void *data)
{
	MonoCompile *cfg;
	MonoJitExceptionInfo *ei;
	guint32 ei_len, i;
	gpointer *type_info;

	cfg = TlsGetValue (current_cfg_tls_id);
	g_assert (cfg);

	/*
	 * data points to a DWARF FDE structure, convert it to our unwind format and
	 * save it.
	 * An alternative would be to save it directly, and modify our unwinder to work
	 * with it.
	 */
	cfg->encoded_unwind_ops = mono_unwind_decode_fde ((guint8*)data, &cfg->encoded_unwind_ops_len, NULL, &ei, &ei_len, &type_info);

	cfg->llvm_ex_info = mono_mempool_alloc0 (cfg->mempool, ei_len * sizeof (MonoJitExceptionInfo));
	cfg->llvm_ex_info_len = ei_len;
	memcpy (cfg->llvm_ex_info, ei, ei_len * sizeof (MonoJitExceptionInfo));
	/* Fill the rest of the information from the type info */
	for (i = 0; i < ei_len; ++i) {
		MonoExceptionClause *clause = type_info [i];

		cfg->llvm_ex_info [i].flags = clause->flags;
		cfg->llvm_ex_info [i].data.catch_class = clause->data.catch_class;
	}

	g_free (ei);
}

static void
add_intrinsics (LLVMModuleRef module)
{
	/* Emit declarations of instrinsics */
	{
		LLVMTypeRef memset_params [] = { LLVMPointerType (LLVMInt8Type (), 0), LLVMInt8Type (), LLVMInt32Type (), LLVMInt32Type () };

		LLVMAddFunction (module, "llvm.memset.i32", LLVMFunctionType (LLVMVoidType (), memset_params, 4, FALSE));
	}

	{
		LLVMTypeRef memcpy_params [] = { LLVMPointerType (LLVMInt8Type (), 0), LLVMPointerType (LLVMInt8Type (), 0), LLVMInt32Type (), LLVMInt32Type () };

		LLVMAddFunction (module, "llvm.memcpy.i32", LLVMFunctionType (LLVMVoidType (), memcpy_params, 4, FALSE));
	}

	{
		LLVMTypeRef params [] = { LLVMDoubleType () };

		LLVMAddFunction (module, "llvm.sin.f64", LLVMFunctionType (LLVMDoubleType (), params, 1, FALSE));
		LLVMAddFunction (module, "llvm.cos.f64", LLVMFunctionType (LLVMDoubleType (), params, 1, FALSE));
		LLVMAddFunction (module, "llvm.sqrt.f64", LLVMFunctionType (LLVMDoubleType (), params, 1, FALSE));

		/* This isn't an intrinsic, instead llvm seems to special case it by name */
		LLVMAddFunction (module, "fabs", LLVMFunctionType (LLVMDoubleType (), params, 1, FALSE));
	}

	{
		LLVMTypeRef membar_params [] = { LLVMInt1Type (), LLVMInt1Type (), LLVMInt1Type (), LLVMInt1Type (), LLVMInt1Type () };

		LLVMAddFunction (module, "llvm.atomic.swap.i32.p0i32", LLVMFunctionType2 (LLVMInt32Type (), LLVMPointerType (LLVMInt32Type (), 0), LLVMInt32Type (), FALSE));
		LLVMAddFunction (module, "llvm.atomic.swap.i64.p0i64", LLVMFunctionType2 (LLVMInt64Type (), LLVMPointerType (LLVMInt64Type (), 0), LLVMInt64Type (), FALSE));
		LLVMAddFunction (module, "llvm.atomic.load.add.i32.p0i32", LLVMFunctionType2 (LLVMInt32Type (), LLVMPointerType (LLVMInt32Type (), 0), LLVMInt32Type (), FALSE));
		LLVMAddFunction (module, "llvm.atomic.load.add.i64.p0i64", LLVMFunctionType2 (LLVMInt64Type (), LLVMPointerType (LLVMInt64Type (), 0), LLVMInt64Type (), FALSE));
		LLVMAddFunction (module, "llvm.atomic.cmp.swap.i32.p0i32", LLVMFunctionType3 (LLVMInt32Type (), LLVMPointerType (LLVMInt32Type (), 0), LLVMInt32Type (), LLVMInt32Type (), FALSE));
		LLVMAddFunction (module, "llvm.atomic.cmp.swap.i64.p0i64", LLVMFunctionType3 (LLVMInt64Type (), LLVMPointerType (LLVMInt64Type (), 0), LLVMInt64Type (), LLVMInt64Type (), FALSE));
		LLVMAddFunction (module, "llvm.memory.barrier", LLVMFunctionType (LLVMVoidType (), membar_params, 5, FALSE));
	}

	{
		LLVMTypeRef ovf_res_i32 [] = { LLVMInt32Type (), LLVMInt1Type () };
		LLVMTypeRef ovf_params_i32 [] = { LLVMInt32Type (), LLVMInt32Type () };

		LLVMAddFunction (module, "llvm.sadd.with.overflow.i32", LLVMFunctionType (LLVMStructType (ovf_res_i32, 2, FALSE), ovf_params_i32, 2, FALSE));
		LLVMAddFunction (module, "llvm.uadd.with.overflow.i32", LLVMFunctionType (LLVMStructType (ovf_res_i32, 2, FALSE), ovf_params_i32, 2, FALSE));
		LLVMAddFunction (module, "llvm.ssub.with.overflow.i32", LLVMFunctionType (LLVMStructType (ovf_res_i32, 2, FALSE), ovf_params_i32, 2, FALSE));
		LLVMAddFunction (module, "llvm.usub.with.overflow.i32", LLVMFunctionType (LLVMStructType (ovf_res_i32, 2, FALSE), ovf_params_i32, 2, FALSE));
		LLVMAddFunction (module, "llvm.smul.with.overflow.i32", LLVMFunctionType (LLVMStructType (ovf_res_i32, 2, FALSE), ovf_params_i32, 2, FALSE));
		LLVMAddFunction (module, "llvm.umul.with.overflow.i32", LLVMFunctionType (LLVMStructType (ovf_res_i32, 2, FALSE), ovf_params_i32, 2, FALSE));
	}

	{
		LLVMTypeRef ovf_res_i64 [] = { LLVMInt64Type (), LLVMInt1Type () };
		LLVMTypeRef ovf_params_i64 [] = { LLVMInt64Type (), LLVMInt64Type () };

		LLVMAddFunction (module, "llvm.sadd.with.overflow.i64", LLVMFunctionType (LLVMStructType (ovf_res_i64, 2, FALSE), ovf_params_i64, 2, FALSE));
		LLVMAddFunction (module, "llvm.uadd.with.overflow.i64", LLVMFunctionType (LLVMStructType (ovf_res_i64, 2, FALSE), ovf_params_i64, 2, FALSE));
		LLVMAddFunction (module, "llvm.ssub.with.overflow.i64", LLVMFunctionType (LLVMStructType (ovf_res_i64, 2, FALSE), ovf_params_i64, 2, FALSE));
		LLVMAddFunction (module, "llvm.usub.with.overflow.i64", LLVMFunctionType (LLVMStructType (ovf_res_i64, 2, FALSE), ovf_params_i64, 2, FALSE));
		LLVMAddFunction (module, "llvm.smul.with.overflow.i64", LLVMFunctionType (LLVMStructType (ovf_res_i64, 2, FALSE), ovf_params_i64, 2, FALSE));
		LLVMAddFunction (module, "llvm.umul.with.overflow.i64", LLVMFunctionType (LLVMStructType (ovf_res_i64, 2, FALSE), ovf_params_i64, 2, FALSE));
	}

	/* EH intrinsics */
	{
		LLVMTypeRef arg_types [2];

		arg_types [0] = LLVMPointerType (LLVMInt8Type (), 0);
		arg_types [1] = LLVMPointerType (LLVMInt8Type (), 0);
		LLVMAddFunction (module, "llvm.eh.selector", LLVMFunctionType (LLVMInt32Type (), arg_types, 2, TRUE));

		LLVMAddFunction (module, "llvm.eh.exception", LLVMFunctionType (LLVMPointerType (LLVMInt8Type (), 0), NULL, 0, FALSE));

		LLVMAddFunction (module, "mono_personality", LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE));

		LLVMAddFunction (module, "mono_resume_unwind", LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE));
	}

	/* SSE intrinsics */
	{
		LLVMTypeRef vector_type, arg_types [2];

		vector_type = LLVMVectorType (LLVMInt32Type (), 4);
		arg_types [0] = vector_type;
		arg_types [1] = vector_type;
		LLVMAddFunction (module, "llvm.x86.sse41.pminud", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					
		LLVMAddFunction (module, "llvm.x86.sse41.pmaxud", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					

		vector_type = LLVMVectorType (LLVMInt16Type (), 8);
		arg_types [0] = vector_type;
		arg_types [1] = vector_type;
		LLVMAddFunction (module, "llvm.x86.sse41.pminuw", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					
		LLVMAddFunction (module, "llvm.x86.sse41.pmaxuw", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					

		vector_type = LLVMVectorType (LLVMInt8Type (), 16);
		arg_types [0] = vector_type;
		arg_types [1] = vector_type;
		LLVMAddFunction (module, "llvm.x86.sse41.pminub", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					
		LLVMAddFunction (module, "llvm.x86.sse41.pmaxub", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					

		vector_type = LLVMVectorType (LLVMDoubleType (), 2);
		arg_types [0] = vector_type;
		arg_types [1] = vector_type;
		LLVMAddFunction (module, "llvm.x86.sse2.min.pd", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					
		LLVMAddFunction (module, "llvm.x86.sse2.max.pd", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					

		vector_type = LLVMVectorType (LLVMFloatType (), 4);
		arg_types [0] = vector_type;
		arg_types [1] = vector_type;
		LLVMAddFunction (module, "llvm.x86.sse2.min.ps", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					
		LLVMAddFunction (module, "llvm.x86.sse2.max.ps", LLVMFunctionType (vector_type, arg_types, 2, FALSE));					
	}
}

void
mono_llvm_init (void)
{
	current_cfg_tls_id = TlsAlloc ();

	jit_module.module = LLVMModuleCreateWithName ("mono");

	ee = mono_llvm_create_ee (LLVMCreateModuleProviderForExistingModule (jit_module.module), alloc_cb, emitted_cb, exception_cb);

	add_intrinsics (jit_module.module);

	jit_module.llvm_types = g_hash_table_new (NULL, NULL);

	LLVMAddGlobalMapping (ee, LLVMGetNamedFunction (jit_module.module, "mono_resume_unwind"), mono_resume_unwind);
}

void
mono_llvm_cleanup (void)
{
	mono_llvm_dispose_ee (ee);

	g_hash_table_destroy (jit_module.llvm_types);
}

void
mono_llvm_create_aot_module (const char *got_symbol)
{
	/* Delete previous module */
	if (aot_module.plt_entries)
		g_hash_table_destroy (aot_module.plt_entries);

	memset (&aot_module, 0, sizeof (aot_module));

	aot_module.module = LLVMModuleCreateWithName ("aot");
	aot_module.got_symbol = got_symbol;

	add_intrinsics (aot_module.module);

	/* Add GOT */
	/*
	 * We couldn't compute the type of the LLVM global representing the got because
	 * its size is only known after all the methods have been emitted. So create
	 * a dummy variable, and replace all uses it with the real got variable when
	 * its size is known in mono_llvm_emit_aot_module ().
	 */
	{
		LLVMTypeRef got_type = LLVMArrayType (IntPtrType (), 0);

		aot_module.got_var = LLVMAddGlobal (aot_module.module, got_type, "mono_dummy_got");
		LLVMSetInitializer (aot_module.got_var, LLVMConstNull (got_type));
	}

	/* Add a method to generate the 'methods' symbol needed by the AOT compiler */
	{
		LLVMValueRef methods_method = LLVMAddFunction (aot_module.module, "methods", LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE));
		LLVMBasicBlockRef bb = LLVMAppendBasicBlock (methods_method, "BB_ENTRY");
		LLVMBuilderRef builder = LLVMCreateBuilder ();
		LLVMPositionBuilderAtEnd (builder, bb);
		LLVMBuildRetVoid (builder);
	}

	/* Add a dummy personality function */
	{
		LLVMBasicBlockRef lbb;
		LLVMBuilderRef lbuilder;
		LLVMValueRef personality;

		personality = LLVMAddFunction (aot_module.module, "mono_aot_personality", LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE));
		LLVMSetLinkage (personality, LLVMPrivateLinkage);
		lbb = LLVMAppendBasicBlock (personality, "BB0");
		lbuilder = LLVMCreateBuilder ();
		LLVMPositionBuilderAtEnd (lbuilder, lbb);
		LLVMBuildRetVoid (lbuilder);
	}

	aot_module.llvm_types = g_hash_table_new (NULL, NULL);
	aot_module.plt_entries = g_hash_table_new (g_str_hash, g_str_equal);
}

/*
 * Emit the aot module into the LLVM bitcode file FILENAME.
 */
void
mono_llvm_emit_aot_module (const char *filename, int got_size)
{
	LLVMTypeRef got_type;
	LLVMValueRef real_got;

	/* 
	 * Create the real got variable and replace all uses of the dummy variable with
	 * the real one.
	 */
	got_type = LLVMArrayType (IntPtrType (), got_size);
	real_got = LLVMAddGlobal (aot_module.module, got_type, aot_module.got_symbol);
	LLVMSetInitializer (real_got, LLVMConstNull (got_type));
	LLVMSetLinkage (real_got, LLVMInternalLinkage);

	mono_llvm_replace_uses_of (aot_module.got_var, real_got);

	mark_as_used (aot_module.module, real_got);

#if 0
	{
		char *verifier_err;

		if (LLVMVerifyModule (aot_module.module, LLVMReturnStatusAction, &verifier_err)) {
			g_assert_not_reached ();
		}
	}
#endif

	LLVMWriteBitcodeToFile (aot_module.module, filename);
}

/*
  DESIGN:
  - Emit LLVM IR from the mono IR using the LLVM C API.
  - The original arch specific code remains, so we can fall back to it if we run
    into something we can't handle.
  FIXME:
  - llvm's PrettyStackTrace class seems to register a signal handler which screws
    up our GC. Also, it calls sigaction () a _lot_ of times instead of just once.
*/

/*  
  A partial list of issues:
  - Handling of opcodes which can throw exceptions.

      In the mono JIT, these are implemented using code like this:
	  method:
      <compare>
	  throw_pos:
	  b<cond> ex_label
	  <rest of code>
      ex_label:
	  push throw_pos - method
	  call <exception trampoline>

	  The problematic part is push throw_pos - method, which cannot be represented
      in the LLVM IR, since it does not support label values.
	  -> this can be implemented in AOT mode using inline asm + labels, but cannot
	  be implemented in JIT mode ?
	  -> a possible but slower implementation would use the normal exception 
      throwing code but it would need to control the placement of the throw code
      (it needs to be exactly after the compare+branch).
	  -> perhaps add a PC offset intrinsics ?

  - efficient implementation of .ovf opcodes.

	  These are currently implemented as:
	  <ins which sets the condition codes>
	  b<cond> ex_label

	  Some overflow opcodes are now supported by LLVM SVN.

  - exception handling, unwinding.
    - SSA is disabled for methods with exception handlers    
	- How to obtain unwind info for LLVM compiled methods ?
	  -> this is now solved by converting the unwind info generated by LLVM
	     into our format.
	- LLVM uses the c++ exception handling framework, while we use our home grown
      code, and couldn't use the c++ one:
      - its not supported under VC++, other exotic platforms.
	  - it might be impossible to support filter clauses with it.

  - trampolines.
  
    The trampolines need a predictable call sequence, since they need to disasm
    the calling code to obtain register numbers / offsets.

    LLVM currently generates this code in non-JIT mode:
	   mov    -0x98(%rax),%eax
	   callq  *%rax
    Here, the vtable pointer is lost. 
    -> solution: use one vtable trampoline per class.

  - passing/receiving the IMT pointer/RGCTX.
    -> solution: pass them as normal arguments ?

  - argument passing.
  
	  LLVM does not allow the specification of argument registers etc. This means
      that all calls are made according to the platform ABI.

  - passing/receiving vtypes.

      Vtypes passed/received in registers are handled by the front end by using
	  a signature with scalar arguments, and loading the parts of the vtype into those
	  arguments.

	  Vtypes passed on the stack are handled using the 'byval' attribute.

  - ldaddr.

    Supported though alloca, we need to emit the load/store code.

  - types.

    The mono JIT uses pointer sized iregs/double fregs, while LLVM uses precisely
    typed registers, so we have to keep track of the precise LLVM type of each vreg.
    This is made easier because the IR is already in SSA form.
    An additional problem is that our IR is not consistent with types, i.e. i32/ia64 
	types are frequently used incorrectly.
*/

/*
  AOT SUPPORT:
  Emit LLVM bytecode into a .bc file, compile it using llc into a .s file, then 
  append the AOT data structures to that file. For methods which cannot be
  handled by LLVM, the normal JIT compiled versions are used.
*/

/* FIXME: Normalize some aspects of the mono IR to allow easier translation, like:
 *   - each bblock should end with a branch
 *   - setting the return value, making cfg->ret non-volatile
 * - merge some changes back to HEAD, to reduce the differences.
 * - avoid some transformations in the JIT which make it harder for us to generate
 *   code.
 * - fix memory leaks.
 * - use pointer types to help optimizations.
 */