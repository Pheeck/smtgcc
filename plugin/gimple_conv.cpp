#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gcc-plugin.h"
#include "tree-pass.h"
#include "tree.h"
#include "tree-cfg.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-pretty-print.h"
#include "print-tree.h"
#include "internal-fn.h"
#include "tree-ssa-operands.h"
#include "ssa.h"
#include "cgraph.h"
#include "alloc-pool.h"
#include "symbol-summary.h"
#include "ipa-prop.h"
#include "value-query.h"
#include "attribs.h"

#include "smtgcc.h"
#include "gimple_conv.h"

static_assert(sizeof(HOST_WIDE_INT) == 8);

// TODO: This is declared in builtins.h, but I get some problems with missing
// includes when I include it.
unsigned int get_object_alignment(tree exp);

// How many bytes load, store, __builtin_memset, etc. can expand.
#define MAX_MEMORY_UNROLL_LIMIT  10000

// Size of anonymous memory size blocks we may need to introduce (for example,
// so that function pointer arguments have memory to point to).
#define ANON_MEM_SIZE 128

// The maximum number of basic blocks allowed in a function.
#define MAX_BBS  1000

// The maximum number of instructions in one basic block.
#define MAX_NOF_INSTS  100000

using namespace std::string_literals;
using namespace smtgcc;

namespace {

struct Addr {
  Instruction *ptr;
  uint64_t bitoffset;
};

struct Converter {
  Converter(Module *module, CommonState *state, function *fun)
    : module{module}
    , state{state}
    , fun{fun}
  {}
  ~Converter()
  {
    if (func)
      destroy_function(func);
  }
  Module *module;
  CommonState *state;
  function *fun;
  Function *func = nullptr;
  std::string pass_name;
  std::map<Basic_block *, std::set<Basic_block *>> switch_bbs;
  std::map<basic_block, Basic_block *> gccbb2bb;
  std::map<Basic_block *, std::pair<Instruction *, Instruction *> > bb2retval;
  std::map<tree, Instruction *> tree2instruction;
  std::map<tree, Instruction *> tree2undef;
  std::map<tree, Instruction *> decl2instruction;
  std::map<Instruction *, Instruction *> inst2memory_flagsx;
  Instruction *retval = nullptr;
  int retval_bitsize;
  tree retval_type;

  Instruction *build_memory_inst(uint64_t id, uint64_t size, uint32_t flags);
  void constrain_range(Basic_block *bb, tree expr, Instruction *inst, Instruction *undef=nullptr);
  std::pair<Instruction *, Instruction *> tree2inst(Basic_block *bb, tree expr);
  Instruction *tree2inst_undefcheck(Basic_block *bb, tree expr);
  Instruction *tree2inst_constructor(Basic_block *bb, tree expr);
  Instruction *add_to_pointer(Basic_block *bb,
				      Instruction *ptr,
				      Instruction *value);
  Addr process_array_ref(Basic_block *bb, tree expr);
  Addr process_component_ref(Basic_block *bb, tree expr);
  Addr process_bit_field_ref(Basic_block *bb, tree expr);
  Addr process_address(Basic_block *bb, tree expr);
  std::pair<Instruction *, Instruction *> vector_as_array(Basic_block *bb, tree expr);
  std::pair<Instruction *, Instruction *> process_load(Basic_block *bb, tree expr);
  void process_store(tree addr_expr, tree value_expr, Basic_block *bb);
  void store_value(Basic_block *bb, Instruction *ptr, Instruction *value);

  Instruction *type_convert(Instruction *inst, tree src_type, tree dest_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_unary_bool(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_type, tree arg1_type, Basic_block *bb);
  Instruction *process_unary_int(enum tree_code code, Instruction *arg1, tree lhs_type, tree arg1_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_unary_int(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_type, tree arg1_type, Basic_block *bb);
  Instruction *process_unary_scalar(enum tree_code code, Instruction *arg1, tree lhs_type, tree arg1_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_unary_scalar(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_type, tree arg1_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_unary_vec(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_elem_type, tree arg1_elem_type, Basic_block *bb);
  Instruction *process_unary_float(enum tree_code code, Instruction *arg1, tree lhs_type, tree arg1_type, Basic_block *bb);
  Instruction *process_unary_complex(enum tree_code code, Instruction *arg1, tree lhs_type, Basic_block *bb);
  Instruction *process_binary_float(enum tree_code code, Instruction *arg1, Instruction *arg2, Basic_block *bb);
  Instruction *process_binary_complex(enum tree_code code, Instruction *arg1, Instruction *arg2, tree lhs_type, Basic_block *bb);
  Instruction *process_binary_complex_cmp(enum tree_code code, Instruction *arg1, Instruction *arg2, tree lhs_type, tree arg1_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_binary_bool(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb);
  Instruction *process_binary_int(enum tree_code code, bool is_unsigned, Instruction *arg1, Instruction *arg2, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_binary_int(enum tree_code code, bool is_unsigned, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb);
  Instruction *process_binary_scalar(enum tree_code code, Instruction *arg1, Instruction *arg2, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_binary_scalar(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_binary_vec(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb);
  Instruction *process_ternary(enum tree_code code, Instruction *arg1, Instruction *arg2, Instruction *arg3, tree arg1_type, tree arg2_type, tree arg3_type, Basic_block *bb);
  Instruction *process_ternary_vec(enum tree_code code, Instruction *arg1, Instruction *arg2, Instruction *arg3, tree lhs_type, tree arg1_type, tree arg2_type, tree arg3_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_vec_cond(Instruction *arg1, Instruction *arg2, Instruction *arg2_undef, Instruction *arg3, Instruction *arg3_undef, tree arg1_type, tree arg2_type, Basic_block *bb);
  std::pair<Instruction *, Instruction *> process_vec_perm_expr(gimple *stmt, Basic_block *bb);

  std::pair<Instruction *, Instruction *> vector_constructor(Basic_block *bb, tree expr);
  void process_constructor(tree lhs, tree rhs, Basic_block *bb);
  void process_gimple_assign(gimple *stmt, Basic_block *bb);
  void process_gimple_asm(gimple *stmt);
  void process_gimple_call_builtin(gimple *stmt, Basic_block *bb);
  void process_gimple_call_internal(gimple *stmt, Basic_block *bb);
  void process_gimple_call(gimple *stmt, Basic_block *bb);
  void process_gimple_return(gimple *stmt, Basic_block *bb);
  Instruction *build_label_cond(tree index_expr, tree label,
					Basic_block *bb);
  void process_gimple_switch(gimple *stmt, Basic_block *bb);
  Basic_block *get_phi_arg_bb(gphi *phi, int i);
  void generate_return_inst(Basic_block *bb);
  void xxx_constructor(tree initial, Instruction *mem_inst);
  void init_var(tree decl, Instruction *mem_inst);
  void make_uninit(Basic_block *bb, Instruction *ptr, uint64_t size);
  void process_variables();
  void process_func_args();
  void process_instructions(int nof_blocks, int *postorder);
  Function *process_function();
};

unsigned __int128 get_widest_int_val(widest_int v)
{
  unsigned int len = v.get_len();
  const HOST_WIDE_INT *p = v.get_val();
  assert(len == 1 || len == 2);
  unsigned __int128 value = 0;
  if (len == 2)
    value = ((unsigned __int128)p[1]) << 64;
  else
    {
      int64_t t = p[0] >> 63;
      value = ((unsigned __int128)t) << 64;
    }
  value |= (uint64_t)p[0];
  return value;
}

unsigned __int128 get_wide_int_val(wide_int v)
{
  unsigned int len = v.get_len();
  const HOST_WIDE_INT *p = v.get_val();
  assert(len == 1 || len == 2);
  unsigned __int128 value = 0;
  if (len == 2)
    value = ((unsigned __int128)p[1]) << 64;
  else
    {
      int64_t t = p[0] >> 63;
      value = ((unsigned __int128)t) << 64;
    }
  value |= (uint64_t)p[0];
  return value;
}

unsigned __int128 get_int_cst_val(tree expr)
{
  assert(TREE_CODE(expr) == INTEGER_CST);
  uint32_t precision = TYPE_PRECISION(TREE_TYPE(expr));
  assert(precision > 0 && precision <= 128);
  unsigned __int128 value = 0;
  if (TREE_INT_CST_NUNITS(expr) == 2)
    {
      value = TREE_INT_CST_ELT(expr, 1);
      value <<= 64;
      value |= (uint64_t)TREE_INT_CST_ELT(expr, 0);
    }
  else
    value = (int64_t)TREE_INT_CST_ELT(expr, 0);
  return value;
}

void check_type(tree type)
{
  // Note: We do not check that all elements in structures/arrays have
  // valid type -- they will be checked when the fields are accessed.
  // This makes us able to analyze progams having invalid elements in
  // unused structures/arrays.
  if (DECIMAL_FLOAT_TYPE_P(type))
    throw Not_implemented("check_type: DECIMAL_FLOAT_TYPE");
  else if (INTEGRAL_TYPE_P(type) && TYPE_PRECISION(type) > 128)
    throw Not_implemented("check_type: integral type with precision > 128");
  else if (VECTOR_TYPE_P(type) || TREE_CODE(type) == COMPLEX_TYPE)
    check_type(TREE_TYPE(type));
  else if (FLOAT_TYPE_P(type))
    {
      // We do not support 80-bit floating point because of two
      // reasons:
      // 1. It is a 128-bit type in memory and registers, so we must
      //    special case it in load/store or for all floating point
      //    operations to chose the correct 128 or 80 bit operation.
      // 2. It does not follow IEEE, so we must do some extra work
      //    to get the correct bits (or report bogus errors when it
      //    is constant folded).
      uint64_t precision = TYPE_PRECISION(type);
      if (precision != 16 && precision != 32 && precision != 64
	  && precision != 128)
	throw Not_implemented("check_type: fp" + std::to_string(precision));
    }
}

// The logical bitsize used in the IR for the GCC type/
uint64_t bitsize_for_type(tree type)
{
  check_type(type);

  if (INTEGRAL_TYPE_P(type))
    return TYPE_PRECISION(type);

  tree size_tree = TYPE_SIZE(type);
  if (size_tree == NULL_TREE)
    throw Not_implemented("bitsize_for_type: incomplete type");
  if (TREE_CODE(size_tree) != INTEGER_CST)
    {
      // Things like function parameters
      //   int foo(int n, struct T { char a[n]; } b);
      throw Not_implemented("bitsize_for_type: dynamically sized type");
    }
  return TREE_INT_CST_LOW(size_tree);
}

// The size of the GCC type when stored in memory etc.
uint64_t bytesize_for_type(tree type)
{
  tree size_tree = TYPE_SIZE(type);
  if (size_tree == NULL_TREE)
    throw Not_implemented("incomplete parameter type");
  if (TREE_CODE(size_tree) != INTEGER_CST)
    {
      // Things like function parameters
      //   int foo(int n, struct T { char a[n]; } b);
      throw Not_implemented("complicated parameter type");
    }
  uint64_t bitsize = TREE_INT_CST_LOW(size_tree);
  assert((bitsize & 7) == 0);
  return bitsize / 8;
}

Instruction *extract_vec_elem(Basic_block *bb, Instruction *inst, uint32_t elem_bitsize, uint32_t idx)
{
  assert(inst->bitsize % elem_bitsize == 0);
  Instruction *high = bb->value_inst(idx * elem_bitsize + elem_bitsize - 1, 32);
  Instruction *low = bb->value_inst(idx * elem_bitsize, 32);
  return bb->build_inst(Op::EXTRACT, inst, high, low);
}

Instruction *extract_elem(Basic_block *bb, Instruction *vec, uint32_t elem_bitsize, Instruction *idx)
{
  Instruction *elm_bsize = bb->value_inst(elem_bitsize, idx->bitsize);
  Instruction *shift = bb->build_inst(Op::MUL, idx, elm_bsize);
  if (shift->bitsize > vec->bitsize)
    {
      Instruction *high = bb->value_inst(vec->bitsize - 1, 32);
      Instruction *low = bb->value_inst(0, 32);
      shift = bb->build_inst(Op::EXTRACT, shift, high, low);
    }
  else if (shift->bitsize < vec->bitsize)
    {
      Instruction *bitsize_inst = bb->value_inst(vec->bitsize, 32);
      shift = bb->build_inst(Op::ZEXT, shift, bitsize_inst);
    }
  Instruction *inst = bb->build_inst(Op::LSHR, vec, shift);
  Instruction *high = bb->value_inst(elem_bitsize - 1, 32);
  Instruction *low = bb->value_inst(0, 32);
  return bb->build_inst(Op::EXTRACT, inst, high, low);
}

// Add a check that floating point bitvectors use the correct NaN value.
//
// This is needed because SMT solvers canonicalizes NaN values. So if we get
// a non-canonical value, then the SMT solver will change the result. This
// does not matter in most cases (as it will change it consistently for
// the source and target) but it fails in e.g., gcc.dg/tree-ssa/mult-abs-2.c
// where we optimize
//   return x * (x > 0.f ? -1.f : 1.f);
// to
//   if (x > 0.f)
//     return -x;
//   else
//     return x;
// so that we do only do the NaN canonicalization in one path.
//
// We therefore add this check each time we may get an non-canonical NaN
// from the outside, and only do the translation validation using NaN
// values that will work.
//
// Note: Floating point in unions cannot be fixed up here...
void canonical_nan_check(Basic_block *bb, Instruction *inst, tree type, Instruction *undef)
{
  // TODO: arrays.
  if (SCALAR_FLOAT_TYPE_P(type))
    {
      Instruction *cond = bb->build_inst(Op::IS_NONCANONICAL_NAN, inst);
      if (undef)
	{
	  // We do not check that NaN are canonical if there are undefined
	  // bits, as that often gives us spurious failures for e.g. partially
	  // created complex numbers. All use of the undef element will be
	  // be flagged as UB when used, so we are not hiding any real issues.
	  Instruction *zero = bb->value_inst(0, undef->bitsize);
	  Instruction *cond2 = bb->build_inst(Op::EQ, undef, zero);
	  cond = bb->build_inst(Op::AND, cond, cond2);
	}
      bb->build_inst(Op::UB, cond);
      return;
    }
  if (TREE_CODE(type) == RECORD_TYPE)
    {
      for (tree fld = TYPE_FIELDS(type); fld; fld = DECL_CHAIN(fld))
	{
	  if (TREE_CODE(fld) != FIELD_DECL)
	    continue;
	  if (DECL_BIT_FIELD_TYPE(fld))
	    continue;
	  tree elem_type = TREE_TYPE(fld);
	  uint64_t elem_size = bytesize_for_type(elem_type);
	  if (elem_size == 0)
	    continue;
	  uint64_t elem_offset = get_int_cst_val(DECL_FIELD_OFFSET(fld));
	  elem_offset += get_int_cst_val(DECL_FIELD_BIT_OFFSET(fld)) / 8;

	  Instruction *high =
	    bb->value_inst((elem_offset + elem_size) * 8 - 1, 32);
	  Instruction *low = bb->value_inst(elem_offset * 8, 32);
	  Instruction *extract = bb->build_inst(Op::EXTRACT, inst, high, low);
	  Instruction *extract2 = nullptr;
	  if (undef)
	    extract2 = bb->build_inst(Op::EXTRACT, undef, high, low);
	  canonical_nan_check(bb, extract, elem_type, extract2);
	}
      return;
    }
  if (VECTOR_TYPE_P(type) || TREE_CODE(type) == COMPLEX_TYPE)
    {
      tree elem_type = TREE_TYPE(type);
      if (!FLOAT_TYPE_P(elem_type))
	return;
      uint32_t elem_bitsize = bitsize_for_type(elem_type);
      uint32_t nof_elt = bitsize_for_type(type) / elem_bitsize;
      for (uint64_t i = 0; i < nof_elt; i++)
	{
	  Instruction *extract = extract_vec_elem(bb, inst, elem_bitsize, i);
	  Instruction *extract2 = nullptr;
	  if (undef)
	    extract2 = extract_vec_elem(bb, undef, elem_bitsize, i);
	  canonical_nan_check(bb, extract, elem_type, extract2);
	}
      return;
    }
}

void constrain_pointer(Basic_block *bb, Instruction *inst, tree type, Instruction *mem_flags)
{
  // TODO: We should invert the meaning of mem_flags.
  // TODO: mem_flags is not correct name -- it is only one flag.
  if (POINTER_TYPE_P(type))
    {
      uint32_t ptr_id_bits = bb->func->module->ptr_id_bits;
      Instruction *id = bb->build_extract_id(inst);
      Instruction *zero = bb->value_inst(0, ptr_id_bits);
      Instruction *cond = bb->build_inst(Op::SLT, id, zero);
      Instruction *not_written = bb->build_extract_id(mem_flags);
      not_written = bb->build_inst(Op::EQ, not_written, zero);
      cond = bb->build_inst(Op::AND, cond, not_written);
      bb->build_inst(Op::UB, cond);
    }

  // TODO: arrays.
  if (TREE_CODE(type) == RECORD_TYPE)
    {
      for (tree fld = TYPE_FIELDS(type); fld; fld = DECL_CHAIN(fld))
	{
	  if (TREE_CODE(fld) != FIELD_DECL)
	    continue;
	  if (DECL_BIT_FIELD_TYPE(fld))
	    continue;
	  tree elem_type = TREE_TYPE(fld);
	  uint64_t elem_size = bytesize_for_type(elem_type);
	  if (elem_size == 0)
	    continue;
	  uint64_t elem_offset = get_int_cst_val(DECL_FIELD_OFFSET(fld));
	  elem_offset += get_int_cst_val(DECL_FIELD_BIT_OFFSET(fld)) / 8;
	  Instruction *high =
	    bb->value_inst((elem_offset + elem_size) * 8 - 1, 32);
	  Instruction *low = bb->value_inst(elem_offset * 8, 32);
	  Instruction *extract = bb->build_inst(Op::EXTRACT, inst, high, low);
	  Instruction *extract2 =
	    bb->build_inst(Op::EXTRACT, mem_flags, high, low);
	  constrain_pointer(bb, extract, elem_type, extract2);
	}
      return;
    }
}

Instruction *Converter::build_memory_inst(uint64_t id, uint64_t size, uint32_t flags)
{
  Basic_block *bb = func->bbs[0];
  uint32_t ptr_id_bits = func->module->ptr_id_bits;
  uint32_t ptr_offset_bits = func->module->ptr_offset_bits;
  Instruction *arg1 = bb->value_inst(id, ptr_id_bits);
  Instruction *arg2 = bb->value_inst(size, ptr_offset_bits);
  Instruction *arg3 = bb->value_inst(flags, 32);
  return bb->build_inst(Op::MEMORY, arg1, arg2, arg3);
}

void build_ub_if_not_zero(Basic_block *bb, Instruction *inst)
{
  Instruction *zero = bb->value_inst(0, inst->bitsize);
  Instruction *cmp = bb->build_inst(Op::NE, inst, zero);
  bb->build_inst(Op::UB, cmp);
}

int popcount(unsigned __int128 x)
{
  int result = 0;
  for (int i = 0; i < 4; i++)
    {
      uint32_t t = x >> (i* 32);
      result += __builtin_popcount(t);
    }
  return result;
}

int clz(unsigned __int128 x)
{
  int result = 0;
  for (int i = 0; i < 4; i++)
    {
      uint32_t t = x >> ((3 - i) * 32);
      if (t)
	return result + __builtin_clz(t);
      result += 32;
    }
  return result;
}

void Converter::constrain_range(Basic_block *bb, tree expr, Instruction *inst, Instruction *undef)
{
  assert(TREE_CODE(expr) == SSA_NAME);

  // The constraints are added when we create a inst for the expr, so the work
  // is already done if tree2instruction contains this expr.
  if (tree2instruction.contains(expr))
    return;

  tree type = TREE_TYPE(expr);
  if (!INTEGRAL_TYPE_P(type) && !POINTER_TYPE_P(type))
    return;

  int_range_max r;
  get_range_query(cfun)->range_of_expr(r, expr);
  if (r.undefined_p() || r.varying_p())
    return;

  // TODO: get_nonzero_bits is deprecated if I understand correctly. This
  // should be updated to the new API.
  Instruction *is_ub1 = nullptr;
  wide_int nz = r.get_nonzero_bits();
  if (nz != -1)
    {
      unsigned __int128 nonzero_bits = get_wide_int_val(nz);
      // The SMT solver get confused, and becomes much slower, when we have
      // both a mask and a range describing the same value. We therefore
      // skip adding a check for the mask if it does not constrain the value
      // more than what the range does.
      // TODO: Implement this for real. For now, we just assume that a mask
      // representing the top n bits as zero is fully represented by the
      // range.
      if (clz(nonzero_bits) + popcount(nonzero_bits) != 128)
	{
	  Instruction *mask = bb->value_inst(~nonzero_bits, inst->bitsize);
	  Instruction *bits = bb->build_inst(Op::AND, inst, mask);
	  Instruction *zero = bb->value_inst(0, bits->bitsize);
	  is_ub1 = bb->build_inst(Op::NE, bits, zero);
	}
    }

  Instruction *is_ub2 = nullptr;
  for (unsigned i = 0; i < r.num_pairs(); i++)
    {
      unsigned __int128 low_val = get_wide_int_val(r.lower_bound(i));
      Instruction *low = bb->value_inst(low_val, inst->bitsize);
      unsigned __int128 high_val = get_wide_int_val(r.upper_bound(i));
      Instruction *high = bb->value_inst(high_val, inst->bitsize);
      Op op = TYPE_UNSIGNED(type) ? Op::UGT : Op::SGT;
      Instruction *cmp_low = bb->build_inst(op, low, inst);
      Instruction *cmp_high = bb->build_inst(op, inst, high);
      Instruction *is_not_in_range = bb->build_inst(Op::OR, cmp_low, cmp_high);
      if (is_ub2)
	is_ub2 = bb->build_inst(Op::AND, is_not_in_range, is_ub2);
      else
	is_ub2 = is_not_in_range;
    }
  assert(is_ub2 != nullptr);

  // Ranges do not take undefined values into account, so, e.g., a phi node
  // may get a range, evem if one of the arguments is undefined. We therefore
  // need to filter out the undef cases from the check, otherwise we will
  // report miscompilation for
  //
  //   int k;
  //   void foo (int x) {
  //     int y;
  //     if (x == 0)
  //       y = 1;
  //     else if (x == 1)
  //       y = 2;
  //     k = y;
  //   }
  //
  // This is safe, because all use of the value that can use the range info
  // will be marked as UB if an undef value is used in that operation.
  if (undef)
    {
      Instruction *zero = bb->value_inst(0, undef->bitsize);
      Instruction *cmp = bb->build_inst(Op::EQ, undef, zero);
      if (is_ub1)
	is_ub1 = bb->build_inst(Op::AND, is_ub1, cmp);
      is_ub2 = bb->build_inst(Op::AND, is_ub2, cmp);
    }

  if (is_ub1)
    bb->build_inst(Op::UB, is_ub1);
  bb->build_inst(Op::UB, is_ub2);
}

void store_ub_check(Basic_block *bb, Instruction *ptr, uint64_t size)
{
  Instruction *size_inst = bb->value_inst(size, ptr->bitsize);
  Instruction *end = bb->build_inst(Op::ADD, ptr, size_inst);
  Instruction *id = bb->build_extract_id(ptr);
  Instruction *id_end = bb->build_extract_id(end);
  Instruction *overflow = bb->build_inst(Op::NE, id, id_end);
  bb->build_inst(Op::UB, overflow);
  Instruction *mem_size = bb->build_inst(Op::GET_MEM_SIZE, id);
  Instruction *offset = bb->build_extract_offset(end);
  Instruction *out_of_bound = bb->build_inst(Op::UGT, offset, mem_size);
  bb->build_inst(Op::UB, out_of_bound);
  Instruction *is_const = bb->build_inst(Op::IS_CONST_MEM, id);
  bb->build_inst(Op::UB, is_const);
}

void load_ub_check(Basic_block *bb, Instruction *ptr, uint64_t size)
{
  Instruction *size_inst = bb->value_inst(size, ptr->bitsize);
  Instruction *end = bb->build_inst(Op::ADD, ptr, size_inst);
  Instruction *id = bb->build_extract_id(ptr);
  Instruction *id_end = bb->build_extract_id(end);
  Instruction *overflow = bb->build_inst(Op::NE, id, id_end);
  bb->build_inst(Op::UB, overflow);
  Instruction *mem_size = bb->build_inst(Op::GET_MEM_SIZE, id);
  Instruction *offset = bb->build_extract_offset(end);
  Instruction *out_of_bound = bb->build_inst(Op::UGT, offset, mem_size);
  bb->build_inst(Op::UB, out_of_bound);
}

Instruction *to_mem_repr(Basic_block *bb, Instruction *inst, tree type)
{
  uint64_t bitsize = bytesize_for_type(type) * 8;
  if (inst->bitsize == bitsize)
    return inst;

  assert(inst->bitsize < bitsize);
  if (INTEGRAL_TYPE_P(type))
    {
      Instruction *bitsize_inst = bb->value_inst(bitsize, 32);
      if (TYPE_UNSIGNED(type))
	inst = bb->build_inst(Op::ZEXT, inst, bitsize_inst);
      else
	inst = bb->build_inst(Op::SEXT, inst, bitsize_inst);
    }
  return inst;
}

// TODO: Are the new bits initialized if the value was uninitialized?
//       What if partially uninitialized?
Instruction *uninit_to_mem_repr(Basic_block *bb, Instruction *inst, tree type)
{
  uint64_t bitsize = bytesize_for_type(type) * 8;
  assert(inst->bitsize <= bitsize);
  if (inst->bitsize != bitsize)
    {
      Instruction *bitsize_inst = bb->value_inst(bitsize, 32);
      inst = bb->build_inst(Op::SEXT, inst, bitsize_inst);
    }
  return inst;
}

// TODO: Imput does not necessaily be mem_repr -- it can be BITFIELD_REF reads
// from vector elements. So should probably be "to_ir_repr" or similar.
Instruction *from_mem_repr(Basic_block *bb, Instruction *inst, tree type)
{
  uint64_t bitsize = bitsize_for_type(type);
  assert(bitsize <= inst->bitsize);
  if (inst->bitsize != bitsize)
    {
      if (TREE_CODE(type) == BOOLEAN_TYPE && bitsize == 1)
	{
	  // A boolean must have value 0 or 1.
	  // TODO: This is a special cases here as this always happen when
	  // storing Booleans in memory. May want to do it in a better way,
	  // and also checking wide Booleans in other places where this
	  // function may be called.
	  Instruction *one = bb->value_inst(1, inst->bitsize);
	  Instruction *cond = bb->build_inst(Op::UGT, inst, one);
	  bb->build_inst(Op::UB, cond);
	}
      inst = bb->build_trunc(inst, bitsize);
    }
  return inst;
}

// Helper function to padding_at_offset.
// TODO: Implement a sane version. And test.
uint8_t bitfield_padding_at_offset(tree fld, int64_t offset)
{
  uint8_t used_bits = 0;
  for (; fld; fld = DECL_CHAIN(fld))
    {
      if (TREE_CODE(fld) != FIELD_DECL)
	continue;

      if (!DECL_BIT_FIELD_TYPE(fld))
	break;

      tree elem_type = TREE_TYPE(fld);
      int64_t elem_bit_size = bitsize_for_type(elem_type);
      if (elem_bit_size == 0)
	continue;

      int64_t elem_size = bytesize_for_type(elem_type);
      int64_t elem_offset = get_int_cst_val(DECL_FIELD_OFFSET(fld));
      int64_t elem_bit_offset = get_int_cst_val(DECL_FIELD_BIT_OFFSET(fld));
      elem_offset += elem_bit_offset / 8;
      elem_bit_offset &= 7;
      elem_size = (elem_bit_offset + elem_bit_size + 7) / 8;
      if (elem_offset <= offset && offset < (elem_offset + elem_size))
	{
	  if (elem_offset < offset)
	    {
	      elem_bit_size -= 8 - elem_bit_offset;
	      elem_bit_offset = 0;
	      elem_offset += 1;
	      if (elem_bit_size < 0)
		continue;
	    }

	  if (elem_offset < offset)
	    {
	      assert(elem_bit_offset == 0);
	      elem_bit_size -= 8 * (offset - elem_offset);
	      if (elem_bit_size < 0)
		continue;
	    }

	  if (elem_bit_size > 8)
	    elem_bit_size = 8;

	  used_bits |= ((1 << elem_bit_size) - 1) << elem_bit_offset;
	}
    }
  return ~used_bits;
}

// Return a bitmask telling which bits are padding (i.e., where the value is
// undefined) for an offset into the type.
uint8_t padding_at_offset(tree type, uint64_t offset)
{
  if (TREE_CODE(type) == ARRAY_TYPE)
    {
      tree elem_type = TREE_TYPE(type);
      uint64_t elem_size = bytesize_for_type(elem_type);
      return padding_at_offset(elem_type, offset % elem_size);
    }
  if (TREE_CODE(type) == RECORD_TYPE)
    {
      for (tree fld = TYPE_FIELDS(type); fld; fld = DECL_CHAIN(fld))
	{
	  if (TREE_CODE(fld) != FIELD_DECL)
	    continue;
	  tree elem_type = TREE_TYPE(fld);
	  uint64_t elem_size = bytesize_for_type(elem_type);
	  uint64_t elem_offset = get_int_cst_val(DECL_FIELD_OFFSET(fld));
	  uint64_t elem_bit_offset =
	    get_int_cst_val(DECL_FIELD_BIT_OFFSET(fld));
	  elem_offset += elem_bit_offset / 8;
	  elem_bit_offset &= 7;
	  if (DECL_BIT_FIELD_TYPE(fld))
	    {
	      uint64_t elem_bit_size = bitsize_for_type(elem_type);
	      elem_size = (elem_bit_offset + elem_bit_size + 7) / 8;
	      if (elem_offset <= offset && offset < (elem_offset + elem_size))
		return bitfield_padding_at_offset(fld, offset);
	    }
	  else if (elem_offset <= offset && offset < (elem_offset + elem_size))
	    return padding_at_offset(elem_type, offset - elem_offset);
	}
      return 0xff;
    }
  if (TREE_CODE(type) == UNION_TYPE)
    {
      // For unions, we mark it as padding if it is padding in all elements.
      uint8_t padding = 0xff;
      for (tree fld = TYPE_FIELDS(type); fld; fld = DECL_CHAIN(fld))
	{
	  tree elem_type = TREE_TYPE(fld);
	  padding &= padding_at_offset(elem_type, offset);
	}
      return padding;
    }

  // The other bytes does not have padding (well, Booleans sort of have
  // padding, but the padding must be 0 so it is not undefined).
  return 0;
}

std::pair<Instruction *, Instruction *> Converter::tree2inst(Basic_block *bb, tree expr)
{
  check_type(TREE_TYPE(expr));

  auto it = tree2instruction.find(expr);
  if (it != tree2instruction.end())
    {
      Instruction *inst = it->second;
      Instruction *undef = nullptr;
      auto it2 = tree2undef.find(expr);
      if (it2 != tree2undef.end())
	undef = it2->second;
      return {inst, undef};
    }

  switch (TREE_CODE(expr))
    {
      case SSA_NAME:
	{
	  tree var = SSA_NAME_VAR(expr);
	  if (var && TREE_CODE(var) == PARM_DECL)
	    {
	      if (tree2instruction.contains(var))
		{
		  Instruction *inst = tree2instruction.at(var);

		  // Place the range check in the entry block as it is
		  // invalid to call the function with invalid values.
		  // This solves the problem that we "randomly" could
		  // mark execution as UB depending on where the param
		  // were used when passes were sinking/hoisting the params.
		  // See gcc.dg/analyzer/pointer-merging.c for a test where
		  // this check makes a difference.
		  constrain_range(func->bbs[0], expr, inst);

		  return {inst, nullptr};
		}
	    }
	  if (var && TREE_CODE(var) == VAR_DECL)
	    {
	      uint64_t bitsize = bitsize_for_type(TREE_TYPE(expr));
	      Instruction *inst = bb->value_inst(0, bitsize);
	      Instruction *undef = bb->value_m1_inst(bitsize);
	      return {inst, undef};
	    }
	  throw Not_implemented("tree2inst: unhandled ssa_name");
	}
    case CONSTRUCTOR:
      if (!VECTOR_TYPE_P(TREE_TYPE(expr)))
	{
	  // Constructors are not supposed to reach this point as they are
	  // only used in
	  //   * store instructions
	  //   * when initializing global variables
	  //   * constructing vectors from scalars
	  throw Not_implemented("tree2inst: constructor");
	}
      return vector_constructor(bb, expr);
    case INTEGER_CST:
      {
	uint32_t precision = bitsize_for_type(TREE_TYPE(expr));
	assert(precision > 0 && precision <= 128);
	unsigned __int128 value = get_int_cst_val(expr);
	Instruction *inst = bb->value_inst(value, precision);
	return {inst, nullptr};
      }
    case REAL_CST:
      {
	tree type = TREE_TYPE(expr);
	check_type(type);
	int nof_bytes = GET_MODE_SIZE(SCALAR_FLOAT_TYPE_MODE(type));
	assert(nof_bytes <= 16);
	long buf[4];
	real_to_target(buf, TREE_REAL_CST_PTR(expr), TYPE_MODE(type));
	union {
	  uint32_t buf[4];
	  unsigned __int128 i;
	} u;
	// real_to_target saves 32 bits in each long, so we copy the
	// values to a uint32_t array to get rid of the extra bits.
	// TODO: Big endian.
	for (int i = 0; i < 4; i++)
	  u.buf[i] = buf[i];
	return {bb->value_inst(u.i, TYPE_PRECISION(type)), nullptr};
      }
    case VECTOR_CST:
      {
	unsigned HOST_WIDE_INT nunits;
	if (!VECTOR_CST_NELTS(expr).is_constant(&nunits))
	  throw Not_implemented("tree2inst: !VECTOR_CST_NELTS");
	Instruction *ret = tree2inst_undefcheck(bb, VECTOR_CST_ELT(expr, 0));
	for (unsigned i = 1; i < nunits; i++)
	  {
	    Instruction *elem =
	      tree2inst_undefcheck(bb, VECTOR_CST_ELT(expr, i));
	    ret = bb->build_inst(Op::CONCAT, elem, ret);
	  }
	return {ret, nullptr};
      }
    case COMPLEX_CST:
      {
	tree elem_type = TREE_TYPE(TREE_TYPE(expr));
	Instruction *real = tree2inst_undefcheck(bb, TREE_REALPART(expr));
	real = to_mem_repr(bb, real, elem_type);
	Instruction *imag = tree2inst_undefcheck(bb, TREE_IMAGPART(expr));
	imag = to_mem_repr(bb, imag, elem_type);
	Instruction *res = bb->build_inst(Op::CONCAT, imag, real);
	return {res, nullptr};
      }
    case IMAGPART_EXPR:
      {
	tree elem_type = TREE_TYPE(expr);
	auto [arg, undef] = tree2inst(bb, TREE_OPERAND(expr, 0));
	Instruction *high = bb->value_inst(arg->bitsize - 1, 32);
	Instruction *low = bb->value_inst(arg->bitsize / 2, 32);
	Instruction *res = bb->build_inst(Op::EXTRACT, arg, high, low);
	res = from_mem_repr(bb, res, elem_type);
	if (undef)
	  {
	    undef = bb->build_inst(Op::EXTRACT, undef, high, low);
	    undef = from_mem_repr(bb, undef, elem_type);
	  }
	return {res, undef};
      }
    case REALPART_EXPR:
      {
	tree elem_type = TREE_TYPE(expr);
	auto [arg, undef] = tree2inst(bb, TREE_OPERAND(expr, 0));
	Instruction *res = bb->build_trunc(arg, arg->bitsize / 2);
	res = from_mem_repr(bb, res, elem_type);
	if (undef)
	  {
	    undef = bb->build_trunc(undef, arg->bitsize / 2);
	    undef = from_mem_repr(bb, undef, elem_type);
	  }
	return {res, undef};
      }
    case VIEW_CONVERT_EXPR:
      {
	auto [arg, undef] = tree2inst(bb, TREE_OPERAND(expr, 0));
	tree src_type = TREE_TYPE(TREE_OPERAND(expr, 0));
	tree dest_type = TREE_TYPE(expr);
	arg = to_mem_repr(bb, arg, src_type);
	arg = from_mem_repr(bb, arg, dest_type);
	if (undef)
	  {
	    undef = uninit_to_mem_repr(bb, undef, src_type);
	    undef = from_mem_repr(bb, undef, dest_type);
	  }
	// TODO: Do we need a local pointer check too?
	canonical_nan_check(bb, arg, dest_type, undef);
	return {arg, undef};
      }
    case ADDR_EXPR:
      {
	Addr addr = process_address(bb, TREE_OPERAND(expr, 0));
	assert(addr.bitoffset == 0);
	return {addr.ptr, nullptr};
      }
    case BIT_FIELD_REF:
      {
	tree arg = TREE_OPERAND(expr, 0);
	auto [value, undef] = tree2inst(bb, arg);
	uint64_t bitsize = get_int_cst_val(TREE_OPERAND(expr, 1));
	uint64_t bit_offset = get_int_cst_val(TREE_OPERAND(expr, 2));
	Instruction *high =
	  bb->value_inst(bitsize + bit_offset - 1, 32);
	Instruction *low = bb->value_inst(bit_offset, 32);
	value = to_mem_repr(bb, value, TREE_TYPE(arg));
	value = bb->build_inst(Op::EXTRACT, value, high, low);
	value = from_mem_repr(bb, value, TREE_TYPE(expr));
	if (undef)
	  {
	    undef = bb->build_inst(Op::EXTRACT, undef, high, low);
	    undef = from_mem_repr(bb, undef, TREE_TYPE(expr));
	  }
	return {value, undef};
      }
    case ARRAY_REF:
      {
	tree array = TREE_OPERAND(expr, 0);
	// Indexing element of a vector as vec[2] is done by an  ARRAY_REF of
	// a VIEW_CONVERT_EXPR of the vector.
	if (TREE_CODE(array) == VIEW_CONVERT_EXPR
	    && VECTOR_TYPE_P(TREE_TYPE(TREE_OPERAND(array, 0))))
	  return vector_as_array(bb, expr);
	return process_load(bb, expr);
      }
    case MEM_REF:
    case COMPONENT_REF:
    case TARGET_MEM_REF:
    case VAR_DECL:
    case RESULT_DECL:
      return process_load(bb, expr);
    default:
      {
	const char *name = get_tree_code_name(TREE_CODE(expr));
	throw Not_implemented("tree2inst: "s + name);
      }
    }
}

Instruction *Converter::tree2inst_undefcheck(Basic_block *bb, tree expr)
{
  auto [inst, undef] = tree2inst(bb, expr);
  if (undef)
    build_ub_if_not_zero(bb, undef);
  return inst;
}

// Processing constructors for global variables may give us more complex expr
// than what we get from normal operations. For example, initializing an array
// of pointers may have an initializer &a-&b that in the function body would
// be calculated by its own stmt.
Instruction *Converter::tree2inst_constructor(Basic_block *bb, tree expr)
{
  check_type(TREE_TYPE(expr));

  auto I = tree2instruction.find(expr);
  if (I != tree2instruction.end())
    return I->second;

  tree_code code = TREE_CODE(expr);
  if (TREE_OPERAND_LENGTH(expr) == 2)
    {
      tree arg1_expr = TREE_OPERAND(expr, 0);
      tree arg2_expr = TREE_OPERAND(expr, 1);
      tree arg1_type = TREE_TYPE(arg1_expr);
      tree arg2_type = TREE_TYPE(arg2_expr);
      Instruction *arg1 = tree2inst_constructor(bb, arg1_expr);
      Instruction *arg2 = tree2inst_constructor(bb, arg2_expr);
      return process_binary_scalar(code, arg1, arg2, TREE_TYPE(expr),
				   arg1_type, arg2_type, bb);
    }
  switch (code)
    {
    case ABS_EXPR:
    case ABSU_EXPR:
    case BIT_NOT_EXPR:
    case NEGATE_EXPR:
    case NOP_EXPR:
    case CONVERT_EXPR:
      {
	tree arg_expr = TREE_OPERAND(expr, 0);
	Instruction *arg = tree2inst_constructor(bb, arg_expr);
	return process_unary_scalar(code, arg, TREE_TYPE(expr), TREE_TYPE(arg_expr), bb);
      }
    default:
      return tree2inst_undefcheck(bb, expr);
    }
}

Instruction *Converter::add_to_pointer(Basic_block *bb, Instruction *ptr, Instruction *value)
{
  if (value->opcode == Op::VALUE && value->value() == 0)
    return ptr;

  Instruction *res = bb->build_inst(Op::ADD, ptr, value);

  // It is UB if the memory id changes by the addition.
  Instruction *id1 = bb->build_extract_id(ptr);
  Instruction *id2 = bb->build_extract_id(res);
  Instruction *is_ub = bb->build_inst(Op::NE, id1, id2);
  bb->build_inst(Op::UB, is_ub);

  // It is UB to add a value with absolute value larger than the
  // offset (this is typically detected by the id check, but that
  // may fail if e.g. the value id bits are -1 and the offset
  // calculation overflows).
  uint32_t ptr_offset_bits = func->module->ptr_offset_bits;
  uint64_t max_off = ((uint64_t)1 << ptr_offset_bits) - 1;
  Instruction *max = bb->value_inst(max_off, value->bitsize);
  Instruction *min = bb->value_inst(-max_off, value->bitsize);
  Instruction *cond1 = bb->build_inst(Op::SGT, value, max);
  Instruction *cond2 = bb->build_inst(Op::SLT, value, min);
  Instruction *cond = bb->build_inst(Op::OR, cond1, cond2);
  bb->build_inst(Op::UB, cond);

  return res;
}

Addr Converter::process_array_ref(Basic_block *bb, tree expr)
{
  tree array = TREE_OPERAND(expr, 0);
  tree index = TREE_OPERAND(expr, 1);
  tree array_type = TREE_TYPE(array);
  tree elem_type = TREE_TYPE(array_type);
  tree domain = TYPE_DOMAIN(array_type);

  Instruction *ptr = process_address(bb, array).ptr;
  Instruction *idx = tree2inst_undefcheck(bb, index);
  if (idx->bitsize < ptr->bitsize)
    {
      Instruction *bitsize_inst = bb->value_inst(ptr->bitsize, 32);
      if (TYPE_UNSIGNED(TREE_TYPE(index)))
	idx = bb->build_inst(Op::ZEXT, idx, bitsize_inst);
      else
	idx = bb->build_inst(Op::SEXT, idx, bitsize_inst);
    }
  else if (idx->bitsize > ptr->bitsize)
    {
      Instruction *high = bb->value_inst(idx->bitsize - 1, 32);
      Instruction *low = bb->value_inst(ptr->bitsize, 32);
      Instruction *top = bb->build_inst(Op::EXTRACT, idx, high, low);
      Instruction *zero = bb->value_inst(0, top->bitsize);
      Instruction *cond = bb->build_inst(Op::NE, top, zero);
      bb->build_inst(Op::UB, cond);
      idx = bb->build_trunc(idx, ptr->bitsize);
    }

  uint64_t elem_size = bytesize_for_type(elem_type);
  Instruction *elm_size = bb->value_inst(elem_size, idx->bitsize);
  Instruction *offset = bb->build_inst(Op::MUL, idx, elm_size);
  ptr = add_to_pointer(bb, ptr, offset);

  Instruction *max_inst = nullptr;
  if (domain && TYPE_MAX_VALUE(domain))
    {
      if (!integer_zerop(TYPE_MIN_VALUE(domain)))
	throw Not_implemented("process_array_ref: index TYPE_MIN_VALUE != 0");
      tree max = TYPE_MAX_VALUE(domain);
      // TODO: Handle variable size arrays. This is (currently) not needed
      //       for correctness -- the array is its own obkect or last in a
      //       strucure, so overflow is detected on the memory block level.
      //       But it will start failing when we support non-constant
      //       field offsets in structures.
      if (TREE_CODE(max) == INTEGER_CST)
	{
	  uint64_t max_val = get_int_cst_val(max);
	  max_inst = bb->value_inst(max_val, idx->bitsize);
	}
    }
  if (max_inst)
    {
      Instruction *cond = bb->build_inst(Op::UGT, idx, max_inst);
      bb->build_inst(Op::UB, cond);
    }
  else
    {
      Op op = TYPE_UNSIGNED(TREE_TYPE(index)) ? Op::ZEXT : Op::SEXT;
      Instruction *ext_bitsize_inst = bb->value_inst(ptr->bitsize * 2, 32);
      Instruction *eidx = bb->build_inst(op, idx, ext_bitsize_inst);
      Instruction *eelm_size = bb->value_inst(elem_size, ptr->bitsize * 2);
      Instruction *eoffset = bb->build_inst(Op::MUL, eidx, eelm_size);
      uint32_t ptr_offset_bits = func->module->ptr_offset_bits;
      Instruction *emax_offset =
	bb->value_inst((uint64_t)1 << ptr_offset_bits, ptr->bitsize * 2);
      Instruction *cond = bb->build_inst(Op::UGE, eoffset, emax_offset);
      bb->build_inst(Op::UB, cond);
    }
  return {ptr, 0};
}

Addr Converter::process_component_ref(Basic_block *bb, tree expr)
{
  tree object = TREE_OPERAND(expr, 0);
  tree field = TREE_OPERAND(expr, 1);

  // TODO: This will need implementation of index checking in variably sized
  //       array too, otherwise we will fail to catch when k is too big in
  //         struct A {int c[k]; int x[n];};
  //       from gcc.dg/pr51628-18.c.
  if (TREE_CODE(DECL_FIELD_OFFSET(field)) != INTEGER_CST)
    throw Not_implemented("process_component_ref: non-constant field offset");
  uint64_t offset = get_int_cst_val(DECL_FIELD_OFFSET(field));
  uint64_t bit_offset = get_int_cst_val(DECL_FIELD_BIT_OFFSET(field));
  offset += bit_offset / 8;
  bit_offset &= 7;

  Instruction *ptr = process_address(bb, object).ptr;
  Instruction *off = bb->value_inst(offset, ptr->bitsize);
  // TODO: This can be a normal ADD when we check that strucutre fits.
  //ptr = bb->build_inst(Op::ADD, ptr, off);
  ptr = add_to_pointer(bb, ptr, off);

  return {ptr, bit_offset};
}

Addr Converter::process_bit_field_ref(Basic_block *bb, tree expr)
{
  tree object = TREE_OPERAND(expr, 0);
  tree position = TREE_OPERAND(expr, 2);
  uint64_t bit_offset = get_int_cst_val(position);
  Instruction *ptr = process_address(bb, object).ptr;
  if (bit_offset > 7)
    {
      uint64_t offset = bit_offset / 8;
      Instruction *off = bb->value_inst(offset, ptr->bitsize);
      ptr = add_to_pointer(bb, ptr, off);
      bit_offset &= 7;
    }
  return {ptr, bit_offset};
}

void alignment_check(Basic_block *bb, tree expr, Instruction *ptr)
{
  // TODO: There are cases where bit_alignment1 and bit_alignment2
  // are inconsistent -- sometimes bit_alignment1 is larges, and
  // sometimes bit_alignment2. And they varies in strange ways.
  // E.g. bit_alignment1 contain info about __builtin_assume_aligned
  // and is often correct in size of type alignment. But sometimes it
  // has the element alignment for vectors. Or 128 when bit_alignment2
  // is 256.
  uint32_t bit_alignment1 = get_object_alignment(expr);
  uint32_t bit_alignment2 = TYPE_ALIGN(TREE_TYPE(expr));
  uint32_t bit_alignment = std::max(bit_alignment1, bit_alignment2);
  assert((bit_alignment1 & 7) == 0);
  assert((bit_alignment2 & 7) == 0);
  uint32_t alignment = bit_alignment / 8;
  if (alignment > 1)
    {
      uint32_t high_val = 0;
      for (;;)
	{
	  high_val++;
	  if (alignment == (1u << high_val))
	    break;
	}

      Instruction *extract = bb->build_trunc(ptr, high_val);
      Instruction *zero = bb->value_inst(0, high_val);
      Instruction *cond = bb->build_inst(Op::NE, extract, zero);
      bb->build_inst(Op::UB, cond);
    }
}

Addr Converter::process_address(Basic_block *bb, tree expr)
{
  tree_code code = TREE_CODE(expr);
  if (code == MEM_REF)
    {
      Instruction *arg1 = tree2inst_undefcheck(bb, TREE_OPERAND(expr, 0));
      Instruction *arg2 = tree2inst_undefcheck(bb, TREE_OPERAND(expr, 1));
      Instruction *ptr = add_to_pointer(bb, arg1, arg2);
      alignment_check(bb, expr, ptr);
      return {ptr, 0};
    }
  if (code == TARGET_MEM_REF)
    {
      // base + (step * index + index2 + offset)
      Instruction *base = tree2inst_undefcheck(bb, TREE_OPERAND(expr, 0));
      Instruction *offset = tree2inst_undefcheck(bb, TREE_OPERAND(expr, 1));
      Instruction *off = offset;
      if (TREE_OPERAND(expr, 2))
	{
	  Instruction *index = tree2inst_undefcheck(bb, TREE_OPERAND(expr, 2));
	  if (TREE_OPERAND(expr, 3))
	    {
	      Instruction *step =
		tree2inst_undefcheck(bb, TREE_OPERAND(expr, 3));
	      index = bb->build_inst(Op::MUL, step, index);
	    }
	  off = bb->build_inst(Op::ADD, off, index);
	}
      if (TREE_OPERAND(expr, 4))
	{
	  Instruction *index2 = tree2inst_undefcheck(bb, TREE_OPERAND(expr, 4));
	  off = bb->build_inst(Op::ADD, off, index2);
	}
      Instruction *ptr = add_to_pointer(bb, base, off);
      alignment_check(bb, expr, ptr);
      return {ptr, 0};
    }
  if (code == VAR_DECL)
    {
      // This check is not needed. But we are currently not adding RTTI
      // structures, which makes the decl2instruction.at(expr) crash for
      // e.g.,  g++.dg/analyzer/pr108003.C
      if (decl2instruction.contains(expr))
	{
	  Instruction *ptr = decl2instruction.at(expr);
	  return {ptr, 0};
	}
    }
  if (code == ARRAY_REF)
    return process_array_ref(bb, expr);
  if (code == COMPONENT_REF)
    return process_component_ref(bb, expr);
  if (code == BIT_FIELD_REF)
    return process_bit_field_ref(bb, expr);
  if (code == VIEW_CONVERT_EXPR)
    return process_address(bb, TREE_OPERAND(expr, 0));
  if (code == REALPART_EXPR)
    return process_address(bb, TREE_OPERAND(expr, 0));
  if (code == IMAGPART_EXPR)
    {
      Instruction *ptr = process_address(bb, TREE_OPERAND(expr, 0)).ptr;
      uint64_t offset_val = bytesize_for_type(TREE_TYPE(expr));
      Instruction *offset = bb->value_inst(offset_val, ptr->bitsize);
      ptr = add_to_pointer(bb, ptr, offset);
      return {ptr, 0};
    }
  if (code == INTEGER_CST)
    {
      Instruction *ptr = tree2inst_undefcheck(bb, expr);
      return {ptr, 0};
    }
  if (code == RESULT_DECL)
    {
      Instruction *ptr = decl2instruction.at(expr);
      return {ptr, 0};
    }

  const char *name = get_tree_code_name(TREE_CODE(expr));
  throw Not_implemented("process_address: "s + name);
}

bool is_bit_field(tree expr)
{
  tree_code code = TREE_CODE(expr);
  if (code == COMPONENT_REF)
    {
      tree field = TREE_OPERAND(expr, 1);
      if (DECL_BIT_FIELD_TYPE(field))
	return true;
    }
  else if (code == BIT_FIELD_REF)
    {
      return true;
    }

  return false;
}

std::pair<Instruction *, Instruction *> Converter::vector_as_array(Basic_block *bb, tree expr)
{
  assert(TREE_CODE(expr) == ARRAY_REF);
  tree array = TREE_OPERAND(expr, 0);
  tree index = TREE_OPERAND(expr, 1);
  tree array_type = TREE_TYPE(array);
  tree elem_type = TREE_TYPE(array_type);
  assert(TREE_CODE(array) == VIEW_CONVERT_EXPR);
  tree vector_expr = TREE_OPERAND(array, 0);
  assert(VECTOR_TYPE_P(TREE_TYPE(vector_expr)));

  auto [inst, undef] = tree2inst(bb, vector_expr);

  uint64_t vector_size = bytesize_for_type(array_type);
  uint64_t elem_size = bytesize_for_type(elem_type);
  assert(vector_size % elem_size == 0);

  Instruction *idx = tree2inst_undefcheck(bb, index);
  Instruction *nof_elems =
    bb->value_inst(vector_size / elem_size, idx->bitsize);
  Instruction *cond = bb->build_inst(Op::UGE, idx, nof_elems);
  bb->build_inst(Op::UB, cond);

  Instruction *elm_bitsize = bb->value_inst(elem_size * 8, idx->bitsize);
  Instruction *shift = bb->build_inst(Op::MUL, idx, elm_bitsize);

  if (inst->bitsize > shift->bitsize)
    {
      Instruction *bitsize_inst = bb->value_inst(inst->bitsize, 32);
      shift = bb->build_inst(Op::ZEXT, shift, bitsize_inst);
    }
  else if (inst->bitsize < shift->bitsize)
    shift = bb->build_trunc(shift, inst->bitsize);
  inst = bb->build_inst(Op::LSHR, inst, shift);
  inst = bb->build_trunc(inst, elem_size * 8);
  inst = from_mem_repr(bb, inst, elem_type);
  if (undef)
    {
      undef = bb->build_inst(Op::LSHR, undef, shift);
      undef = bb->build_trunc(undef, elem_size * 8);
      undef = from_mem_repr(bb, undef, elem_type);
    }

  return {inst, undef};
}

std::pair<Instruction *, Instruction *> Converter::process_load(Basic_block *bb, tree expr)
{
  tree type = TREE_TYPE(expr);
  uint64_t bitsize = bitsize_for_type(type);
  uint64_t size = bytesize_for_type(type);
  if (bitsize == 0)
    throw Not_implemented("tree2inst: load unhandled size 0");
  if (size > MAX_MEMORY_UNROLL_LIMIT)
    throw Not_implemented("tree2inst: load size too big");
  Addr addr = process_address(bb, expr);
  bool is_bitfield = is_bit_field(expr);
  assert(is_bitfield || addr.bitoffset == 0);
  if (is_bitfield)
    size = (bitsize + addr.bitoffset + 7) / 8;
  load_ub_check(bb, addr.ptr, size);
  Instruction *value = nullptr;
  Instruction *undef = nullptr;
  Instruction *mem_flags2 = nullptr;
  for (uint64_t i = 0; i < size; i++)
    {
      Instruction *offset = bb->value_inst(i, addr.ptr->bitsize);
      Instruction *ptr = bb->build_inst(Op::ADD, addr.ptr, offset);

      Instruction *data_byte;
      Instruction *undef_byte;
      uint8_t padding = padding_at_offset(type, i);
      if (padding == 255)
	{
	  // No need to load a value as its value is indeterminate.
	  data_byte = bb->value_inst(0, 8);
	  undef_byte = bb->value_inst(255, 8);
	}
      else
	{
	  data_byte = bb->build_inst(Op::LOAD, ptr);
	  undef_byte = bb->build_inst(Op::GET_MEM_UNDEF, ptr);
	  if (padding != 0)
	    {
	      Instruction *padding_inst = bb->value_inst(padding, 8);
	      undef_byte = bb->build_inst(Op::OR, undef_byte, padding_inst);
	    }
	}

      if (value)
	value = bb->build_inst(Op::CONCAT, data_byte, value);
      else
	value = data_byte;
      if (undef)
	undef = bb->build_inst(Op::CONCAT, undef_byte, undef);
      else
	undef = undef_byte;

      // TODO: Rename. This is not mem_flags -- we only splats one flag.
      Instruction *flag = bb->build_inst(Op::GET_MEM_FLAG, ptr);
      flag = bb->build_inst(Op::SEXT, flag, bb->value_inst(8, 32));
      if (mem_flags2)
	mem_flags2 = bb->build_inst(Op::CONCAT, flag, mem_flags2);
      else
	mem_flags2 = flag;
    }
  if (is_bitfield)
    {
      Instruction *high = bb->value_inst(bitsize + addr.bitoffset - 1, 32);
      Instruction *low = bb->value_inst(addr.bitoffset, 32);
      value = bb->build_inst(Op::EXTRACT, value, high, low);
      undef = bb->build_inst(Op::EXTRACT, undef, high, low);
      mem_flags2 = bb->build_inst(Op::EXTRACT, mem_flags2, high, low);
    }
  else
    {
      value = from_mem_repr(bb, value, TREE_TYPE(expr));
      // TODO: What if the extracted bits are defined, but the extra bits
      // undefined?
      // E.g. a bool where the least significant bit is defined, but the rest
      // undefined. I guess it should be undefined?
      undef = from_mem_repr(bb, undef, TREE_TYPE(expr));
      mem_flags2 = from_mem_repr(bb, mem_flags2, TREE_TYPE(expr));
      inst2memory_flagsx[value] = mem_flags2;
    }

  constrain_pointer(bb, value, TREE_TYPE(expr), mem_flags2);
  canonical_nan_check(bb, value, TREE_TYPE(expr), undef);

  return {value, undef};
}

// Write value to memory. No UB checks etc. are done, and memory flags/uninit
// are not updated.
void Converter::store_value(Basic_block *bb, Instruction *ptr, Instruction *value)
{
  if ((value->bitsize & 7) != 0)
    throw Not_implemented("store_value: not byte aligned");
  uint64_t size = value->bitsize / 8;
  Instruction *one = bb->value_inst(1, ptr->bitsize);
  for (uint64_t i = 0; i < size; i++)
    {
      Instruction *high = bb->value_inst(i * 8 + 7, 32);
      Instruction *low = bb->value_inst(i * 8, 32);
      Instruction *byte = bb->build_inst(Op::EXTRACT, value, high, low);
      bb->build_inst(Op::STORE, ptr, byte);
      ptr = bb->build_inst(Op::ADD, ptr, one);
    }
}

void Converter::process_store(tree addr_expr, tree value_expr, Basic_block *bb)
{
  if (TREE_CODE(value_expr) == STRING_CST)
    {
      uint64_t str_len = TREE_STRING_LENGTH(value_expr);
      uint64_t size = bytesize_for_type(TREE_TYPE(addr_expr));
      assert(str_len <= size);
      const char *p = TREE_STRING_POINTER(value_expr);
      Addr ptr_addr = process_address(bb, addr_expr);
      assert(!ptr_addr.bitoffset);
      Instruction *ptr = ptr_addr.ptr;
      Instruction *one = bb->value_inst(1, ptr->bitsize);
      Instruction *memory_flag = bb->value_inst(1, 1);
      Instruction *undef = bb->value_inst(0, 8);
      if (size > MAX_MEMORY_UNROLL_LIMIT)
	throw Not_implemented("process_gimple_assign: too large string");

      store_ub_check(bb, ptr, size);
      for (uint64_t i = 0; i < size; i++)
	{
	  uint8_t byte = (i < str_len) ? p[i] : 0;
	  Instruction *value = bb->value_inst(byte, 8);
	  bb->build_inst(Op::STORE, ptr, value);
	  bb->build_inst(Op::SET_MEM_FLAG, ptr, memory_flag);
	  bb->build_inst(Op::SET_MEM_UNDEF, ptr, undef);
	  ptr = bb->build_inst(Op::ADD, ptr, one);
	}
      return;
    }

  tree value_type = TREE_TYPE(value_expr);
  bool is_bitfield = is_bit_field(addr_expr);
  Addr addr = process_address(bb, addr_expr);
  assert(is_bitfield || addr.bitoffset == 0);
  assert(addr.bitoffset < 8);
  auto [value, undef] = tree2inst(bb, value_expr);
  if (!undef)
    undef = bb->value_inst(0, value->bitsize);

  uint64_t size;
  if (is_bitfield)
    {
      uint64_t bitsize = bitsize_for_type(value_type);
      size = (bitsize + addr.bitoffset + 7) / 8;

      if (addr.bitoffset)
	{
	  Instruction *first_byte = bb->build_inst(Op::LOAD, addr.ptr);
	  Instruction *bits = bb->build_trunc(first_byte, addr.bitoffset);
	  value = bb->build_inst(Op::CONCAT, value, bits);

	  first_byte = bb->build_inst(Op::GET_MEM_UNDEF, addr.ptr);
	  bits = bb->build_trunc(first_byte, addr.bitoffset);
	  undef = bb->build_inst(Op::CONCAT, undef, bits);
	}

      if (bitsize + addr.bitoffset != size * 8)
	{
	  Instruction *offset = bb->value_inst(size - 1, addr.ptr->bitsize);
	  Instruction *ptr = bb->build_inst(Op::ADD, addr.ptr, offset);

	  uint64_t remaining = size * 8 - (bitsize + addr.bitoffset);
	  assert(remaining < 8);
	  Instruction *high = bb->value_inst(7, 32);
	  Instruction *low = bb->value_inst(8 - remaining, 32);

	  Instruction *last_byte = bb->build_inst(Op::LOAD, ptr);
	  Instruction *bits = bb->build_inst(Op::EXTRACT, last_byte, high, low);
	  value = bb->build_inst(Op::CONCAT, bits, value);

	  last_byte = bb->build_inst(Op::GET_MEM_UNDEF, ptr);
	  bits = bb->build_inst(Op::EXTRACT, last_byte, high, low);
	  undef = bb->build_inst(Op::CONCAT, bits, undef);
	}
    }
  else
    {
      size = bytesize_for_type(value_type);
      value = to_mem_repr(bb, value, value_type);
      undef = uninit_to_mem_repr(bb, undef, value_type);
    }

  // TODO: Adjust for bitfield?
  Instruction *memory_flagsx = nullptr;
  if (inst2memory_flagsx.contains(value))
    memory_flagsx = inst2memory_flagsx.at(value);

  for (uint64_t i = 0; i < size; i++)
    {
      Instruction *offset = bb->value_inst(i, addr.ptr->bitsize);
      Instruction *ptr = bb->build_inst(Op::ADD, addr.ptr, offset);

      Instruction *high = bb->value_inst(i * 8 + 7, 32);
      Instruction *low = bb->value_inst(i * 8, 32);

      uint8_t padding = padding_at_offset(value_type, i);
      if (padding == 255)
	{
	  // No need to store if this is padding as it will be marked as
	  // undefined anyway.
	  bb->build_inst(Op::SET_MEM_UNDEF, ptr, bb->value_inst(255, 8));
	}
      else
	{
	  Instruction *byte = bb->build_inst(Op::EXTRACT, value, high, low);
	  bb->build_inst(Op::STORE, ptr, byte);

	  byte = bb->build_inst(Op::EXTRACT, undef, high, low);
	  if (padding != 0)
	    {
	      Instruction *padding_inst = bb->value_inst(padding, 8);
	      byte = bb->build_inst(Op::OR, byte, padding_inst);
	    }
	  bb->build_inst(Op::SET_MEM_UNDEF, ptr, byte);
	}

      Instruction *memory_flag;
      if (memory_flagsx)
	{
	  memory_flag = bb->build_inst(Op::EXTRACT, memory_flagsx, high, low);
	  Instruction *zero = bb->value_inst(0, memory_flag->bitsize);
	  memory_flag = bb->build_inst(Op::NE, memory_flag, zero);
	}
      else
	memory_flag = bb->value_inst(1, 1);
      bb->build_inst(Op::SET_MEM_FLAG, ptr, memory_flag);
    }

  store_ub_check(bb, addr.ptr, size);
}

// Convert a scalar value of src_type to dest_type.
Instruction *Converter::type_convert(Instruction *inst, tree src_type, tree dest_type, Basic_block *bb)
{
  if (TREE_CODE(dest_type) == BOOLEAN_TYPE)
    {
      assert(INTEGRAL_TYPE_P(src_type));
      if (inst->bitsize > 1)
	inst = bb->build_extract_bit(inst, 0);
      unsigned dest_prec = bitsize_for_type(dest_type);
      if (dest_prec == 1)
	return inst;
      Op op = TYPE_UNSIGNED(dest_type) ? Op::ZEXT : Op::SEXT;
      Instruction *dest_prec_inst = bb->value_inst(dest_prec, 32);
      return bb->build_inst(op, inst, dest_prec_inst);
    }

  if (INTEGRAL_TYPE_P(src_type) || POINTER_TYPE_P(src_type) || TREE_CODE(src_type) == OFFSET_TYPE)
    {
      if (INTEGRAL_TYPE_P(dest_type) || POINTER_TYPE_P(dest_type) || TREE_CODE(dest_type) == OFFSET_TYPE)
	{
	  unsigned src_prec = inst->bitsize;
	  unsigned dest_prec = bitsize_for_type(dest_type);
	  if (src_prec > dest_prec)
	    return bb->build_trunc(inst, dest_prec);
	  if (src_prec == dest_prec)
	    return inst;
	  Op op = TYPE_UNSIGNED(src_type) ? Op::ZEXT : Op::SEXT;
	  Instruction *dest_prec_inst = bb->value_inst(dest_prec, 32);
	  return bb->build_inst(op, inst, dest_prec_inst);
	}
      if (FLOAT_TYPE_P(dest_type))
	{
	  unsigned dest_prec = TYPE_PRECISION(dest_type);
	  Instruction *dest_prec_inst = bb->value_inst(dest_prec, 32);
	  Op op = TYPE_UNSIGNED(src_type) ? Op::U2F : Op::S2F;
	  return bb->build_inst(op, inst, dest_prec_inst);
	}
    }

  if (FLOAT_TYPE_P(src_type))
    {
      if (TREE_CODE(dest_type) == INTEGER_TYPE
	  || TREE_CODE(dest_type) == ENUMERAL_TYPE)
	{
	  // The result is UB if the floating point value is out of range
	  // for the integer.
	  // TODO: This is OK for float precsion <= dest precision.
	  // But for float precision > dest we currently mark as UB cases
	  // that round into range.
	  Instruction *min =
	    tree2inst_undefcheck(bb, TYPE_MIN_VALUE(dest_type));
	  Instruction *max =
	    tree2inst_undefcheck(bb, TYPE_MAX_VALUE(dest_type));
	  Op op = TYPE_UNSIGNED(dest_type) ? Op::U2F : Op::S2F;
	  int src_bitsize = TYPE_PRECISION(src_type);
	  Instruction *src_bitsize_inst = bb->value_inst(src_bitsize, 32);
	  Instruction *fmin = bb->build_inst(op, min, src_bitsize_inst);
	  Instruction *fmax = bb->build_inst(op, max, src_bitsize_inst);
	  Instruction *clow = bb->build_inst(Op::FGE, inst, fmin);
	  Instruction *chigh = bb->build_inst(Op::FLE, inst, fmax);
	  Instruction *is_in_range = bb->build_inst(Op::AND, clow, chigh);
	  Instruction *is_ub = bb->build_inst(Op::NOT, is_in_range);
	  bb->build_inst(Op::UB, is_ub);

	  int dest_bitsize = bitsize_for_type(dest_type);
	  op = TYPE_UNSIGNED(dest_type) ? Op::F2U : Op::F2S;
	  Instruction *dest_bitsize_inst = bb->value_inst(dest_bitsize, 32);
	  return bb->build_inst(op, inst, dest_bitsize_inst);
	}
      if (FLOAT_TYPE_P(dest_type))
	{
	  unsigned src_prec = TYPE_PRECISION(src_type);
	  unsigned dest_prec = TYPE_PRECISION(dest_type);
	  if (src_prec == dest_prec)
	    return inst;
	  Instruction *dest_prec_inst = bb->value_inst(dest_prec, 32);
	  return bb->build_inst(Op::FCHPREC, inst, dest_prec_inst);
	}
    }

  throw Not_implemented("type_convert: unknown type");
}

void check_wide_bool(Instruction *inst, tree type, Basic_block *bb)
{
  Instruction *false_inst = bb->value_inst(0, inst->bitsize);
  Instruction *true_inst = bb->value_inst(1, inst->bitsize);
  if (!TYPE_UNSIGNED(type))
    true_inst = bb->build_inst(Op::NEG, true_inst);
  Instruction *cond0 = bb->build_inst(Op::NE, inst, true_inst);
  Instruction *cond1 = bb->build_inst(Op::NE, inst, false_inst);
  Instruction *cond = bb->build_inst(Op::AND, cond0, cond1);
  bb->build_inst(Op::UB, cond);
}

std::pair<Instruction *, Instruction *> Converter::process_unary_bool(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  assert(TREE_CODE(lhs_type) == BOOLEAN_TYPE);

  auto [lhs, lhs_undef] =
    process_unary_int(code, arg1, arg1_undef, lhs_type, arg1_type, bb);

  if (lhs->bitsize > 1)
    check_wide_bool(lhs, lhs_type, bb);

  assert(lhs->bitsize == TYPE_PRECISION(lhs_type));
  return {lhs, lhs_undef};
}

Instruction *Converter::process_unary_int(enum tree_code code, Instruction *arg1, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  switch (code)
    {
    case ABS_EXPR:
      {
	if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	  {
	    unsigned __int128 min_int =
	      ((unsigned __int128)1) << (arg1->bitsize - 1);
	    Instruction *min_int_inst =
	      bb->value_inst(min_int, arg1->bitsize);
	    Instruction *cond =
	      bb->build_inst(Op::EQ, arg1, min_int_inst);
	    bb->build_inst(Op::UB, cond);
	  }
	assert(!TYPE_UNSIGNED(arg1_type));
	Instruction *neg = bb->build_inst(Op::NEG, arg1);
	Instruction *zero = bb->value_inst(0, arg1->bitsize);
	Instruction *cond = bb->build_inst(Op::SGE, arg1, zero);
	return bb->build_inst(Op::ITE, cond, arg1, neg);
      }
    case ABSU_EXPR:
      {
	assert(!TYPE_UNSIGNED(arg1_type));
	Instruction *neg = bb->build_inst(Op::NEG, arg1);
	Instruction *zero = bb->value_inst(0, arg1->bitsize);
	Instruction *cond = bb->build_inst(Op::SGE, arg1, zero);
	return bb->build_inst(Op::ITE, cond, arg1, neg);
      }
    case BIT_NOT_EXPR:
      return bb->build_inst(Op::NOT, arg1);
    case FIX_TRUNC_EXPR:
      return type_convert(arg1, arg1_type, lhs_type, bb);
    case NEGATE_EXPR:
      if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	{
	  unsigned __int128 min_int =
	    ((unsigned __int128)1) << (arg1->bitsize - 1);
	  Instruction *min_int_inst =
	    bb->value_inst(min_int, arg1->bitsize);
	  Instruction *cond =
	    bb->build_inst(Op::EQ, arg1, min_int_inst);
	  bb->build_inst(Op::UB, cond);
	}
      return bb->build_inst(Op::NEG, arg1);
    case CONVERT_EXPR:
    case NOP_EXPR:
      return type_convert(arg1, arg1_type, lhs_type, bb);
    default:
      throw Not_implemented("process_unary_int: "s + get_tree_code_name(code));
    }
}

std::pair<Instruction *, Instruction *> Converter::process_unary_int(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  // Handle instructions that accept uninitialized arguments.
  switch (code)
    {
    case BIT_NOT_EXPR:
      return {bb->build_inst(Op::NOT, arg1), arg1_undef};
    case CONVERT_EXPR:
    case NOP_EXPR:
      if (INTEGRAL_TYPE_P(arg1_type) && INTEGRAL_TYPE_P(lhs_type))
	{
	  unsigned dest_prec = bitsize_for_type(lhs_type);
	  if (dest_prec == arg1->bitsize)
	    return {arg1, arg1_undef};
	  else if (dest_prec < arg1->bitsize)
	    {
	      arg1 = bb->build_trunc(arg1, dest_prec);
	      if (arg1_undef)
		arg1_undef = bb->build_trunc(arg1_undef, dest_prec);
	      return {arg1, arg1_undef};
	    }
	}
      break;
    default:
      break;
    }

  // Handle instructions where uninitialized arguments are UB.
  if (arg1_undef)
    build_ub_if_not_zero(bb, arg1_undef);
  Instruction *res = process_unary_int(code, arg1, lhs_type, arg1_type, bb);
  return {res, nullptr};
}

Instruction *Converter::process_unary_float(enum tree_code code, Instruction *arg1, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  switch (code)
    {
    case ABS_EXPR:
      return bb->build_inst(Op::FABS, arg1);
    case FLOAT_EXPR:
      return type_convert(arg1, arg1_type, lhs_type, bb);
    case NEGATE_EXPR:
      return bb->build_inst(Op::FNEG, arg1);
    case CONVERT_EXPR:
    case NOP_EXPR:
      return type_convert(arg1, arg1_type, lhs_type, bb);
    case PAREN_EXPR:
      return arg1;
    default:
      throw Not_implemented("process_unary_float: "s + get_tree_code_name(code));
    }
}

Instruction *Converter::process_unary_complex(enum tree_code code, Instruction *arg1, tree lhs_type, Basic_block *bb)
{
  tree elem_type = TREE_TYPE(lhs_type);
  uint64_t bitsize = arg1->bitsize;
  uint64_t elem_bitsize = bitsize / 2;
  Instruction *real_high = bb->value_inst(elem_bitsize - 1, 32);
  Instruction *real_low = bb->value_inst(0, 32);
  Instruction *imag_high = bb->value_inst(bitsize - 1, 32);
  Instruction *imag_low = bb->value_inst(elem_bitsize, 32);
  Instruction *arg1_real =
    bb->build_inst(Op::EXTRACT, arg1, real_high, real_low);
  arg1_real = from_mem_repr(bb, arg1_real, elem_type);
  Instruction *arg1_imag =
    bb->build_inst(Op::EXTRACT, arg1, imag_high, imag_low);
  arg1_imag = from_mem_repr(bb, arg1_imag, elem_type);

  switch (code)
    {
    case CONJ_EXPR:
      {
	Instruction *inst_imag;
	inst_imag = process_unary_scalar(NEGATE_EXPR, arg1_imag,
					 elem_type, elem_type, bb);
	arg1_real = to_mem_repr(bb, arg1_real, elem_type);
	inst_imag = to_mem_repr(bb, inst_imag, elem_type);
	return bb->build_inst(Op::CONCAT, inst_imag, arg1_real);
      }
    case NEGATE_EXPR:
      {
	Instruction * inst_real =
	  process_unary_scalar(code, arg1_real, elem_type, elem_type, bb);
	Instruction *inst_imag =
	  process_unary_scalar(code, arg1_imag, elem_type, elem_type, bb);
	inst_real = to_mem_repr(bb, inst_real, elem_type);
	inst_imag = to_mem_repr(bb, inst_imag, elem_type);
	return bb->build_inst(Op::CONCAT, inst_imag, inst_real);
      }
    default:
      throw Not_implemented("process_unary_complex: "s + get_tree_code_name(code));
    }
}

Instruction *Converter::process_unary_scalar(enum tree_code code, Instruction *arg1, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  if (TREE_CODE(lhs_type) == BOOLEAN_TYPE)
    {
      auto [inst, undef] = process_unary_bool(code, arg1, nullptr, lhs_type,
					      arg1_type, bb);
      assert(!undef);
      return inst;
    }
  else if (FLOAT_TYPE_P(lhs_type))
    return process_unary_float(code, arg1, lhs_type, arg1_type, bb);
  else
    return process_unary_int(code, arg1, lhs_type, arg1_type, bb);
}

std::pair<Instruction *, Instruction *> Converter::process_unary_scalar(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  if (TREE_CODE(lhs_type) == BOOLEAN_TYPE)
    return process_unary_bool(code, arg1, arg1_undef, lhs_type, arg1_type, bb);
  else if (FLOAT_TYPE_P(lhs_type))
    {
      if (arg1_undef)
	build_ub_if_not_zero(bb, arg1_undef);
      Instruction *res = process_unary_float(code, arg1, lhs_type,
					     arg1_type, bb);
      return {res, nullptr};
    }
  else
    return process_unary_int(code, arg1, arg1_undef, lhs_type, arg1_type, bb);
}

std::pair<Instruction *, Instruction *> Converter::process_unary_vec(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, tree lhs_elem_type, tree arg1_elem_type, Basic_block *bb)
{
  uint32_t elem_bitsize = bitsize_for_type(arg1_elem_type);
  uint32_t nof_elt = arg1->bitsize / elem_bitsize;
  uint32_t start_idx = 0;

  if (code == VEC_UNPACK_LO_EXPR
      || code == VEC_UNPACK_HI_EXPR
      || code == VEC_UNPACK_FLOAT_LO_EXPR
      || code == VEC_UNPACK_FLOAT_HI_EXPR)
    {
      if (code == VEC_UNPACK_HI_EXPR || code == VEC_UNPACK_FLOAT_HI_EXPR)
	start_idx = nof_elt / 2;
      else
	nof_elt = nof_elt / 2;
      code = CONVERT_EXPR;
    }

  Instruction *res = nullptr;
  Instruction *res_undef = nullptr;
  for (uint64_t i = start_idx; i < nof_elt; i++)
    {
      Instruction *a1_undef = nullptr;
      Instruction *a1 = extract_vec_elem(bb, arg1, elem_bitsize, i);
      if (arg1_undef)
	a1_undef = extract_vec_elem(bb, arg1_undef, elem_bitsize, i);
      auto [inst, inst_undef] =
	process_unary_scalar(code, a1, a1_undef, lhs_elem_type,
			     arg1_elem_type, bb);

      if (res)
	res = bb->build_inst(Op::CONCAT, inst, res);
      else
	res = inst;

      if (arg1_undef)
	{
	  if (res_undef)
	    res_undef = bb->build_inst(Op::CONCAT, inst_undef, res_undef);
	  else
	    res_undef = inst_undef;
	}
    }
  return {res, res_undef};
}

Instruction *Converter::process_binary_float(enum tree_code code, Instruction *arg1, Instruction *arg2, Basic_block *bb)
{
  switch (code)
    {
    case EQ_EXPR:
      return bb->build_inst(Op::FEQ, arg1, arg2);
    case NE_EXPR:
      return bb->build_inst(Op::FNE, arg1, arg2);
    case GE_EXPR:
      return bb->build_inst(Op::FGE, arg1, arg2);
    case GT_EXPR:
      return bb->build_inst(Op::FGT, arg1, arg2);
    case LE_EXPR:
      return bb->build_inst(Op::FLE, arg1, arg2);
    case LT_EXPR:
      return bb->build_inst(Op::FLT, arg1, arg2);
    case UNEQ_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	Instruction *isnan = bb->build_inst(Op::OR, isnan1, isnan2);
	Instruction *cmp = bb->build_inst(Op::FEQ, arg1, arg2);
	return bb->build_inst(Op::OR, isnan, cmp);
      }
    case UNLT_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	Instruction *isnan = bb->build_inst(Op::OR, isnan1, isnan2);
	Instruction *cmp = bb->build_inst(Op::FLT, arg1, arg2);
	return bb->build_inst(Op::OR, isnan, cmp);
      }
    case UNLE_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	Instruction *isnan = bb->build_inst(Op::OR, isnan1, isnan2);
	Instruction *cmp = bb->build_inst(Op::FLE, arg1, arg2);
	return bb->build_inst(Op::OR, isnan, cmp);
      }
    case UNGT_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	Instruction *isnan = bb->build_inst(Op::OR, isnan1, isnan2);
	Instruction *cmp = bb->build_inst(Op::FGT, arg1, arg2);
	return bb->build_inst(Op::OR, isnan, cmp);
      }
    case UNGE_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	Instruction *isnan = bb->build_inst(Op::OR, isnan1, isnan2);
	Instruction *cmp = bb->build_inst(Op::FGE, arg1, arg2);
	return bb->build_inst(Op::OR, isnan, cmp);
      }
    case UNORDERED_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	return bb->build_inst(Op::OR, isnan1, isnan2);
      }
    case ORDERED_EXPR:
      {
	Instruction *isnan1 = bb->build_inst(Op::FNE, arg1, arg1);
	Instruction *isnan2 = bb->build_inst(Op::FNE, arg2, arg2);
	Instruction *isnan = bb->build_inst(Op::OR, isnan1, isnan2);
	return bb->build_inst(Op::NOT, isnan);
      }
    case LTGT_EXPR:
      {
	Instruction *lt = bb->build_inst(Op::FLT, arg1, arg2);
	Instruction *gt = bb->build_inst(Op::FGT, arg1, arg2);
	return bb->build_inst(Op::OR, lt, gt);
      }
    case RDIV_EXPR:
      return bb->build_inst(Op::FDIV, arg1, arg2);
    case MINUS_EXPR:
      return bb->build_inst(Op::FSUB, arg1, arg2);
    case MULT_EXPR:
      return bb->build_inst(Op::FMUL, arg1, arg2);
    case PLUS_EXPR:
      return bb->build_inst(Op::FADD, arg1, arg2);
    default:
      throw Not_implemented("process_binary_float: "s + get_tree_code_name(code));
    }
}

Instruction *Converter::process_binary_complex(enum tree_code code, Instruction *arg1, Instruction *arg2, tree lhs_type, Basic_block *bb)
{
  tree elem_type = TREE_TYPE(lhs_type);
  uint64_t bitsize = arg1->bitsize;
  uint64_t elem_bitsize = bitsize / 2;
  Instruction *real_high = bb->value_inst(elem_bitsize - 1, 32);
  Instruction *real_low = bb->value_inst(0, 32);
  Instruction *imag_high = bb->value_inst(bitsize - 1, 32);
  Instruction *imag_low = bb->value_inst(elem_bitsize, 32);
  Instruction *arg1_real =
    bb->build_inst(Op::EXTRACT, arg1, real_high, real_low);
  arg1_real = from_mem_repr(bb, arg1_real, elem_type);
  Instruction *arg1_imag =
    bb->build_inst(Op::EXTRACT, arg1, imag_high, imag_low);
  arg1_imag = from_mem_repr(bb, arg1_imag, elem_type);
  Instruction *arg2_real =
    bb->build_inst(Op::EXTRACT, arg2, real_high, real_low);
  arg2_real = from_mem_repr(bb, arg2_real, elem_type);
  Instruction *arg2_imag =
    bb->build_inst(Op::EXTRACT, arg2, imag_high, imag_low);
  arg2_imag = from_mem_repr(bb, arg2_imag, elem_type);

  switch (code)
    {
    case MINUS_EXPR:
    case PLUS_EXPR:
      {
	Instruction *inst_real =
	  process_binary_scalar(code, arg1_real, arg2_real,
				elem_type, elem_type, elem_type, bb);
	Instruction *inst_imag =
	  process_binary_scalar(code, arg1_imag, arg2_imag,
				elem_type, elem_type, elem_type, bb);
	inst_real = to_mem_repr(bb, inst_real, elem_type);
	inst_imag = to_mem_repr(bb, inst_imag, elem_type);
	return bb->build_inst(Op::CONCAT, inst_imag, inst_real);
      }
    default:
      throw Not_implemented("process_binary_complex: "s + get_tree_code_name(code));
    }
}

Instruction *Converter::process_binary_complex_cmp(enum tree_code code, Instruction *arg1, Instruction *arg2, tree lhs_type, tree arg1_type, Basic_block *bb)
{
  tree elem_type = TREE_TYPE(arg1_type);
  uint64_t bitsize = arg1->bitsize;
  uint64_t elem_bitsize = bitsize / 2;
  Instruction *real_high = bb->value_inst(elem_bitsize - 1, 32);
  Instruction *real_low = bb->value_inst(0, 32);
  Instruction *imag_high = bb->value_inst(bitsize - 1, 32);
  Instruction *imag_low = bb->value_inst(elem_bitsize, 32);
  Instruction *arg1_real =
    bb->build_inst(Op::EXTRACT, arg1, real_high, real_low);
  arg1_real = from_mem_repr(bb, arg1_real, elem_type);
  Instruction *arg1_imag =
    bb->build_inst(Op::EXTRACT, arg1, imag_high, imag_low);
  arg1_imag = from_mem_repr(bb, arg1_imag, elem_type);
  Instruction *arg2_real =
    bb->build_inst(Op::EXTRACT, arg2, real_high, real_low);
  arg2_real = from_mem_repr(bb, arg2_real, elem_type);
  Instruction *arg2_imag =
    bb->build_inst(Op::EXTRACT, arg2, imag_high, imag_low);
  arg2_imag = from_mem_repr(bb, arg2_imag, elem_type);

  switch (code)
    {
    case EQ_EXPR:
    case NE_EXPR:
      {
	Instruction *cmp_real =
	  process_binary_scalar(code, arg1_real, arg2_real,
				lhs_type, elem_type, elem_type, bb);
	Instruction *cmp_imag =
	  process_binary_scalar(code, arg1_imag, arg2_imag,
				lhs_type, elem_type, elem_type, bb);
	Instruction *cmp;
	if (code == EQ_EXPR)
	  cmp = bb->build_inst(Op::AND, cmp_real, cmp_imag);
	else
	  cmp = bb->build_inst(Op::OR, cmp_real, cmp_imag);
	return cmp;
      }
    default:
      throw Not_implemented("process_binary_complex_cmp: "s + get_tree_code_name(code));
    }
}

std::pair<Instruction *, Instruction *> Converter::process_binary_bool(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  assert(TREE_CODE(lhs_type) == BOOLEAN_TYPE);

  Instruction *lhs;
  Instruction *lhs_undef = nullptr;
  if (FLOAT_TYPE_P(arg1_type))
    lhs = process_binary_float(code, arg1, arg2, bb);
  else
    std::tie(lhs, lhs_undef) =
      process_binary_int(code, TYPE_UNSIGNED(arg1_type), arg1, arg1_undef,
			 arg2, arg2_undef, lhs_type, arg1_type, arg2_type, bb);

  // GCC may use non-standard Boolean types (such as signed-boolean:8), so
  // we may need to extend the value if we have generated a standard 1-bit
  // Boolean for a comparison.
  uint64_t precision = TYPE_PRECISION(lhs_type);
  if (lhs->bitsize == 1 && precision > 1)
    {
      Instruction *bitsize_inst = bb->value_inst(precision, 32);
      Op op = TYPE_UNSIGNED(lhs_type) ? Op::ZEXT : Op::SEXT;
      lhs = bb->build_inst(op, lhs, bitsize_inst);
      if (lhs_undef)
	lhs_undef = bb->build_inst(op, lhs_undef, bitsize_inst);
    }
  if (lhs->bitsize > 1)
    check_wide_bool(lhs, lhs_type, bb);

  assert(lhs->bitsize == precision);
  return {lhs, lhs_undef};
}

Instruction *Converter::process_binary_int(enum tree_code code, bool is_unsigned, Instruction *arg1, Instruction *arg2, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  switch (code)
    {
    case EQ_EXPR:
      return bb->build_inst(Op::EQ, arg1, arg2);
    case NE_EXPR:
      return bb->build_inst(Op::NE, arg1, arg2);
    case GE_EXPR:
      return bb->build_inst(is_unsigned ? Op::UGE : Op::SGE, arg1, arg2);
     case GT_EXPR:
       return bb->build_inst(is_unsigned ? Op::UGT : Op::SGT, arg1, arg2);
    case LE_EXPR:
      return bb->build_inst(is_unsigned ? Op::ULE : Op::SLE, arg1, arg2);
    case LT_EXPR:
      return bb->build_inst(is_unsigned ? Op::ULT : Op::SLT, arg1, arg2);
    case BIT_AND_EXPR:
      return bb->build_inst(Op::AND, arg1, arg2);
    case BIT_IOR_EXPR:
      return bb->build_inst(Op::OR, arg1, arg2);
    case BIT_XOR_EXPR:
      return bb->build_inst(Op::XOR, arg1, arg2);
    case EXACT_DIV_EXPR:
      {
	if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	  {
	    unsigned __int128 min_int =
	      ((unsigned __int128)1) << (arg1->bitsize - 1);
	    Instruction *min_int_inst = bb->value_inst(min_int, arg1->bitsize);
	    Instruction *minus1_inst = bb->value_inst(-1, arg1->bitsize);
	    Instruction *cond1 = bb->build_inst(Op::EQ, arg1, min_int_inst);
	    Instruction *cond2 = bb->build_inst(Op::EQ, arg2, minus1_inst);
	    Instruction *ub_cond = bb->build_inst(Op::AND, cond1, cond2);
	    bb->build_inst(Op::UB, ub_cond);
	  }
	Instruction *zero = bb->value_inst(0, arg1->bitsize);
	Op rem_op = is_unsigned ? Op::UREM : Op::SREM;
	Instruction *rem = bb->build_inst(rem_op, arg1, arg2);
	Instruction *ub_cond = bb->build_inst(Op::NE, rem, zero);
	bb->build_inst(Op::UB, ub_cond);
	Instruction *ub_cond2 = bb->build_inst(Op::EQ, arg2, zero);
	bb->build_inst(Op::UB, ub_cond2);
	Op div_op = is_unsigned ? Op::UDIV : Op::SDIV;
	return bb->build_inst(div_op, arg1, arg2);
      }
    case LSHIFT_EXPR:
      {
	Instruction *bitsize = bb->value_inst(arg1->bitsize, arg2->bitsize);
	Instruction *cond = bb->build_inst(Op::UGE, arg2, bitsize);
	bb->build_inst(Op::UB, cond);
      }
      arg2 = type_convert(arg2, arg2_type, arg1_type, bb);
      return bb->build_inst(Op::SHL, arg1, arg2);
    case MAX_EXPR:
      {
	Op op = is_unsigned ? Op::UMAX : Op::SMAX;
	return bb->build_inst(op, arg1, arg2);
      }
    case MIN_EXPR:
      {
	Op op = is_unsigned ? Op::UMIN : Op::SMIN;
	return bb->build_inst(op, arg1, arg2);
      }
    case MINUS_EXPR:
      if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	{
	  Instruction *cond = bb->build_inst(Op::SSUB_WRAPS, arg1, arg2);
	  bb->build_inst(Op::UB, cond);
	}
      return bb->build_inst(Op::SUB, arg1, arg2);
    case MULT_EXPR:
      if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	{
	  Instruction *cond = bb->build_inst(Op::SMUL_WRAPS, arg1, arg2);
	  bb->build_inst(Op::UB, cond);
	}
      return bb->build_inst(Op::MUL, arg1, arg2);
    case PLUS_EXPR:
      if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	{
	  Instruction *cond = bb->build_inst(Op::SADD_WRAPS, arg1, arg2);
	  bb->build_inst(Op::UB, cond);
	}
      return bb->build_inst(Op::ADD, arg1, arg2);
    case POINTER_DIFF_EXPR:
      {
	// Pointers are treated as unsigned, and the result must fit in
	// a signed integer of the same width.
	assert(arg1->bitsize == arg2->bitsize);
	Instruction *ext_bitsize_inst = bb->value_inst(arg1->bitsize + 1, 32);
	Instruction *earg1 = bb->build_inst(Op::ZEXT, arg1, ext_bitsize_inst);
	Instruction *earg2 = bb->build_inst(Op::ZEXT, arg2, ext_bitsize_inst);
	Instruction *eres = bb->build_inst(Op::SUB, earg1, earg2);
	int bitsize = arg1->bitsize;
	Instruction *etop_bit_idx = bb->value_inst(bitsize, 32);
	Instruction *etop_bit =
	  bb->build_inst(Op::EXTRACT, eres, etop_bit_idx, etop_bit_idx);
	Instruction *top_bit_idx = bb->value_inst(bitsize - 1, 32);
	Instruction *top_bit =
	  bb->build_inst(Op::EXTRACT, eres, top_bit_idx, top_bit_idx);
	Instruction *cmp = bb->build_inst(Op::NE, top_bit, etop_bit);
	bb->build_inst(Op::UB, cmp);
	return bb->build_trunc(eres, bitsize);
      }
    case POINTER_PLUS_EXPR:
      {
	arg2 = type_convert(arg2, arg2_type, arg1_type, bb);
	Instruction *ptr = bb->build_inst(Op::ADD, arg1, arg2);

	{
	  Instruction *id1 = bb->build_extract_id(arg1);
	  Instruction *id2 = bb->build_extract_id(ptr);
	  Instruction *is_ub = bb->build_inst(Op::NE, id1, id2);
	  bb->build_inst(Op::UB, is_ub);
	}

	// The documentation is a bit unclear what "overflow" really mean.
	// Pointers are unsigned, and POINTER_PLUS_EXPR is used for
	// subtraction too, so "p - 1" will always overflow in some sense.
	// So we interpret the addition as a subtraction if the second
	// operand is interpreted as a negative number.
	if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	{
	  Instruction *sub_overflow = bb->build_inst(Op::UGT, ptr, arg1);
	  Instruction *add_overflow = bb->build_inst(Op::ULT, ptr, arg1);
	  Instruction *zero = bb->value_inst(0, arg2->bitsize);
	  Instruction *is_sub = bb->build_inst(Op::SLT, arg2, zero);
	  Instruction *is_ub =
	    bb->build_inst(Op::ITE, is_sub, sub_overflow, add_overflow);
	  bb->build_inst(Op::UB, is_ub);
	}

	// The resulting pointer cannot be NULL if arg1 or arg2 is
	// non-zero, but GIMPLE allows NULL + 0.
	{
	  Instruction *zero = bb->value_inst(0, ptr->bitsize);
	  Instruction *cond1 = bb->build_inst(Op::EQ, ptr, zero);
	  Instruction *cond2 = bb->build_inst(Op::NE, arg1, zero);
	  Instruction *cond3 = bb->build_inst(Op::NE, arg2, zero);
	  Instruction *args_nonzero = bb->build_inst(Op::OR, cond2, cond3);
	  Instruction *cond = bb->build_inst(Op::AND, cond1, args_nonzero);
	  bb->build_inst(Op::UB, cond);
	}
	return ptr;
      }
    case RROTATE_EXPR:
      {
	Instruction *bitsize = bb->value_inst(arg1->bitsize, arg2->bitsize);
	Instruction *cond = bb->build_inst(Op::UGE, arg2, bitsize);
	bb->build_inst(Op::UB, cond);
	arg2 = type_convert(arg2, arg2_type, arg1_type, bb);
	Instruction *concat = bb->build_inst(Op::CONCAT, arg1, arg1);
	Instruction *bitsize_inst = bb->value_inst(concat->bitsize, 32);
	Instruction *shift = bb->build_inst(Op::ZEXT, arg2, bitsize_inst);
	Instruction *shifted = bb->build_inst(Op::LSHR, concat, shift);
	return bb->build_trunc(shifted, arg1->bitsize);
      }
    case LROTATE_EXPR:
      {
	Instruction *bitsize = bb->value_inst(arg1->bitsize, arg2->bitsize);
	Instruction *cond = bb->build_inst(Op::UGE, arg2, bitsize);
	bb->build_inst(Op::UB, cond);
	arg2 = type_convert(arg2, arg2_type, arg1_type, bb);
	Instruction *concat = bb->build_inst(Op::CONCAT, arg1, arg1);
	Instruction *bitsize_inst = bb->value_inst(concat->bitsize, 32);
	Instruction *shift = bb->build_inst(Op::ZEXT, arg2, bitsize_inst);
	Instruction *shifted = bb->build_inst(Op::SHL, concat, shift);
	Instruction *high = bb->value_inst(2 * arg1->bitsize - 1, 32);
	Instruction *low = bb->value_inst(arg1->bitsize, 32);
	return bb->build_inst(Op::EXTRACT, shifted, high, low);
      }
    case RSHIFT_EXPR:
      {
	Instruction *bitsize = bb->value_inst(arg1->bitsize, arg2->bitsize);
	Instruction *cond = bb->build_inst(Op::UGE, arg2, bitsize);
	bb->build_inst(Op::UB, cond);
	Op op = is_unsigned ? Op::LSHR : Op::ASHR;
	arg2 = type_convert(arg2, arg2_type, arg1_type, bb);
	return bb->build_inst(op, arg1, arg2);
      }
    case TRUNC_DIV_EXPR:
      {
	if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	  {
	    unsigned __int128 min_int =
	      ((unsigned __int128)1) << (arg1->bitsize - 1);
	    Instruction *min_int_inst = bb->value_inst(min_int, arg1->bitsize);
	    Instruction *minus1_inst = bb->value_inst(-1, arg1->bitsize);
	    Instruction *cond1 = bb->build_inst(Op::EQ, arg1, min_int_inst);
	    Instruction *cond2 = bb->build_inst(Op::EQ, arg2, minus1_inst);
	    Instruction *cond = bb->build_inst(Op::AND, cond1, cond2);
	    bb->build_inst(Op::UB, cond);
	  }
	Instruction *zero_inst = bb->value_inst(0, arg1->bitsize);
	Instruction *cond = bb->build_inst(Op::EQ, arg2, zero_inst);
	bb->build_inst(Op::UB, cond);
	Op op = is_unsigned ? Op::UDIV : Op::SDIV;
	return bb->build_inst(op, arg1, arg2);
      }
    case TRUNC_MOD_EXPR:
      {
	if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	  {
	    unsigned __int128 min_int =
	      ((unsigned __int128)1) << (arg1->bitsize - 1);
	    Instruction *min_int_inst = bb->value_inst(min_int, arg1->bitsize);
	    Instruction *minus1_inst = bb->value_inst(-1, arg1->bitsize);
	    Instruction *cond1 = bb->build_inst(Op::EQ, arg1, min_int_inst);
	    Instruction *cond2 = bb->build_inst(Op::EQ, arg2, minus1_inst);
	    Instruction *cond = bb->build_inst(Op::AND, cond1, cond2);
	    bb->build_inst(Op::UB, cond);
	  }
	Instruction *zero_inst = bb->value_inst(0, arg1->bitsize);
	Instruction *cond = bb->build_inst(Op::EQ, arg2, zero_inst);
	bb->build_inst(Op::UB, cond);
	Op op = is_unsigned ? Op::UREM : Op::SREM;
	return bb->build_inst(op, arg1, arg2);
      }
    case WIDEN_MULT_EXPR:
      {
	assert(arg1->bitsize == arg2->bitsize);
	assert(TYPE_UNSIGNED(arg1_type) == TYPE_UNSIGNED(arg2_type));
	Instruction *new_bitsize_inst = bb->value_inst(2 * arg1->bitsize, 32);
	Op op = is_unsigned ? Op::ZEXT : Op::SEXT;
	arg1 = bb->build_inst(op, arg1, new_bitsize_inst);
	arg2 = bb->build_inst(op, arg2, new_bitsize_inst);
	return bb->build_inst(Op::MUL, arg1, arg2);
      }
    case MULT_HIGHPART_EXPR:
      {
	assert(arg1->bitsize == arg2->bitsize);
	assert(TYPE_UNSIGNED(arg1_type) == TYPE_UNSIGNED(arg2_type));
	Instruction *new_bitsize_inst = bb->value_inst(2 * arg1->bitsize, 32);
	Op op = is_unsigned ? Op::ZEXT : Op::SEXT;
	arg1 = bb->build_inst(op, arg1, new_bitsize_inst);
	arg2 = bb->build_inst(op, arg2, new_bitsize_inst);
	Instruction *mul = bb->build_inst(Op::MUL, arg1, arg2);
	Instruction *high = bb->value_inst(mul->bitsize - 1, 32);
	Instruction *low = bb->value_inst(mul->bitsize / 2, 32);
	return bb->build_inst(Op::EXTRACT, mul, high, low);
      }
    default:
      throw Not_implemented("process_binary_int: "s + get_tree_code_name(code));
    }
}

std::pair<Instruction *, Instruction *> Converter::process_binary_int(enum tree_code code, bool is_unsigned, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  // Handle instructions that accept uninitialized arguments.
  switch (code)
    {
    case BIT_AND_EXPR:
      {
	Instruction *res = bb->build_inst(Op::AND, arg1, arg2);
	Instruction *res_undef = nullptr;

	if (arg1_undef || arg2_undef)
	  {
	    if (!arg1_undef)
	      arg1_undef = bb->value_inst(0, arg1->bitsize);
	    if (!arg2_undef)
	      arg2_undef = bb->value_inst(0, arg2->bitsize);

	    // 0 & uninitialized is 0.
	    // 1 & uninitialized is uninitialized.
	    Instruction *mask =
	      bb->build_inst(Op::AND,
			     bb->build_inst(Op::OR, arg1, arg1_undef),
			     bb->build_inst(Op::OR, arg2, arg2_undef));
	    res_undef =
	      bb->build_inst(Op::AND,
			     bb->build_inst(Op::OR, arg1_undef, arg2_undef),
			     mask);
	  }

	return {res, res_undef};
      }
    case BIT_IOR_EXPR:
      {
	Instruction *res = bb->build_inst(Op::OR, arg1, arg2);
	Instruction *res_undef = nullptr;

	if (arg1_undef || arg2_undef)
	  {
	    if (!arg1_undef)
	      arg1_undef = bb->value_inst(0, arg1->bitsize);
	    if (!arg2_undef)
	      arg2_undef = bb->value_inst(0, arg2->bitsize);

	    //  0 | uninitialized is uninitialized.
	    // 1 | uninitialized is 1.
	    Instruction *mask =
	      bb->build_inst(Op::AND,
			     bb->build_inst(Op::OR,
					    bb->build_inst(Op::NOT, arg1),
					    arg1_undef),
			     bb->build_inst(Op::OR,
					    bb->build_inst(Op::NOT, arg2),
					    arg2_undef));
	    res_undef =
	      bb->build_inst(Op::AND,
			     bb->build_inst(Op::OR, arg1_undef, arg2_undef),
			     mask);
	  }

	return {res, res_undef};
      }
    case MULT_EXPR:
      {
	Instruction *res_undef = nullptr;
	if (arg1_undef || arg2_undef)
	  {
	    Instruction *zero = bb->value_inst(0, arg1->bitsize);
	    if (!arg1_undef)
	      arg1_undef = zero;
	    if (!arg2_undef)
	      arg2_undef = zero;

	    // The result is defined if no input is uninitizled, or if one of
	    // that arguments is a initialized zero.
	    Instruction *arg1_unini = bb->build_inst(Op::NE, arg1_undef, zero);
	    Instruction *arg1_nonzero = bb->build_inst(Op::NE, arg1, zero);
	    Instruction *arg2_unini = bb->build_inst(Op::NE, arg2_undef, zero);
	    Instruction *arg2_nonzero = bb->build_inst(Op::NE, arg2, zero);
	    Instruction *ub =
	      bb->build_inst(Op::OR,
			     bb->build_inst(Op::AND,
					    arg1_unini,
					    bb->build_inst(Op::OR, arg2_unini,
							   arg2_nonzero)),
			     bb->build_inst(Op::AND,
					    arg2_unini,
					    bb->build_inst(Op::OR, arg1_unini,
							   arg1_nonzero)));
	    res_undef =
	      bb->build_inst(Op::SEXT, ub, bb->value_inst(arg1->bitsize, 32));
	  }

	if (!TYPE_OVERFLOW_WRAPS(lhs_type))
	  {
	    Instruction *cond = bb->build_inst(Op::SMUL_WRAPS, arg1, arg2);
	    bb->build_inst(Op::UB, cond);
	  }
	Instruction *res = bb->build_inst(Op::MUL, arg1, arg2);
	return {res, res_undef};
      }
      default:
	break;
    }

  // Handle instructions where uninitialized arguments are UB.
  if (arg1_undef)
    build_ub_if_not_zero(bb, arg1_undef);
  if (arg2_undef)
    build_ub_if_not_zero(bb, arg2_undef);
  Instruction *res = process_binary_int(code, is_unsigned, arg1, arg2,
					lhs_type, arg1_type, arg2_type, bb);
  return {res, nullptr};
}

Instruction *Converter::process_binary_scalar(enum tree_code code, Instruction *arg1, Instruction *arg2, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  if (TREE_CODE(lhs_type) == BOOLEAN_TYPE)
    {
      auto [inst, undef] =
	process_binary_bool(code, arg1, nullptr, arg2, nullptr, lhs_type,
			    arg1_type, arg2_type, bb);
      assert(!undef);
      return inst;
    }
  else if (FLOAT_TYPE_P(lhs_type))
    return process_binary_float(code, arg1, arg2, bb);
  else
    return process_binary_int(code, TYPE_UNSIGNED(arg1_type), arg1, arg2,
			      lhs_type, arg1_type, arg2_type, bb);
}

std::pair<Instruction *, Instruction *> Converter::process_binary_scalar(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  if (TREE_CODE(lhs_type) == BOOLEAN_TYPE)
    return process_binary_bool(code, arg1, arg1_undef, arg2, arg2_undef,
			       lhs_type, arg1_type, arg2_type, bb);
  else if (FLOAT_TYPE_P(lhs_type))
    {
      if (arg1_undef)
	build_ub_if_not_zero(bb, arg1_undef);
      if (arg2_undef)
	build_ub_if_not_zero(bb, arg2_undef);
      Instruction *res = process_binary_float(code, arg1, arg2, bb);
      return {res, nullptr};
    }
  else
    return process_binary_int(code, TYPE_UNSIGNED(arg1_type),
			      arg1, arg1_undef, arg2, arg2_undef,
			      lhs_type, arg1_type, arg2_type, bb);
}

std::pair<Instruction *, Instruction *> Converter::process_binary_vec(enum tree_code code, Instruction *arg1, Instruction *arg1_undef, Instruction *arg2, Instruction *arg2_undef, tree lhs_type, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  assert(VECTOR_TYPE_P(lhs_type));
  assert(VECTOR_TYPE_P(arg1_type));
  tree lhs_elem_type = TREE_TYPE(lhs_type);
  tree arg1_elem_type = TREE_TYPE(arg1_type);
  tree arg2_elem_type;
  if (VECTOR_TYPE_P(arg2_type))
    arg2_elem_type = TREE_TYPE(arg2_type);
  else
    arg2_elem_type = arg2_type;

  if (code == VEC_PACK_TRUNC_EXPR || code == VEC_PACK_FIX_TRUNC_EXPR)
    {
      if (arg1_undef)
	build_ub_if_not_zero(bb, arg1_undef);
      if (arg2_undef)
	build_ub_if_not_zero(bb, arg2_undef);

      Instruction *arg = bb->build_inst(Op::CONCAT, arg2, arg1);
      return process_unary_vec(CONVERT_EXPR, arg, nullptr, lhs_elem_type,
			       arg1_elem_type, bb);
    }

  uint32_t elem_bitsize = bitsize_for_type(arg1_elem_type);
  uint32_t nof_elt = bitsize_for_type(arg1_type) / elem_bitsize;
  uint32_t start_idx = 0;

  if (code == VEC_WIDEN_MULT_LO_EXPR || code == VEC_WIDEN_MULT_HI_EXPR)
    {
      if (code == VEC_WIDEN_MULT_HI_EXPR)
	start_idx = nof_elt / 2;
      else
	nof_elt = nof_elt / 2;
      code = WIDEN_MULT_EXPR;
    }

  Instruction *res = nullptr;
  Instruction *res_undef = nullptr;
  for (uint64_t i = start_idx; i < nof_elt; i++)
    {
      Instruction *a1_undef = nullptr;
      Instruction *a2_undef = nullptr;
      Instruction *a1 = extract_vec_elem(bb, arg1, elem_bitsize, i);
      if (arg1_undef)
	a1_undef = extract_vec_elem(bb, arg1_undef, elem_bitsize, i);
      Instruction *a2;
      if (VECTOR_TYPE_P(arg2_type))
	{
	  a2 = extract_vec_elem(bb, arg2, elem_bitsize, i);
	  if (arg2_undef)
	    a2_undef = extract_vec_elem(bb, arg2_undef, elem_bitsize, i);
	}
      else
	{
	  a2 = arg2;
	  if (arg2_undef)
	    a2_undef = arg2_undef;
	}
      auto [inst, inst_undef] =
	process_binary_scalar(code, a1, a1_undef, a2, a2_undef, lhs_elem_type,
			      arg1_elem_type, arg2_elem_type, bb);
      if (res)
	res = bb->build_inst(Op::CONCAT, inst, res);
      else
	res = inst;

      if (arg1_undef || arg2_undef)
	{
	  if (res_undef)
	    res_undef = bb->build_inst(Op::CONCAT, inst_undef, res_undef);
	  else
	    res_undef = inst_undef;
	}
    }
  return {res, res_undef};
}

Instruction *Converter::process_ternary(enum tree_code code, Instruction *arg1, Instruction *arg2, Instruction *arg3, tree arg1_type, tree arg2_type, tree arg3_type, Basic_block *bb)
{
  switch (code)
    {
    case SAD_EXPR:
      {
	arg1 = type_convert(arg1, arg1_type, arg3_type, bb);
	arg2 = type_convert(arg2, arg2_type, arg3_type, bb);
	Instruction *inst = bb->build_inst(Op::SUB, arg1, arg2);
	Instruction *zero = bb->value_inst(0, inst->bitsize);
	Instruction *cmp = bb->build_inst(Op::SGE, inst, zero);
	Instruction *neg = bb->build_inst(Op::NEG, inst);
	inst = bb->build_inst(Op::ITE, cmp, inst, neg);
	return bb->build_inst(Op::ADD, inst, arg3);
      }
    case DOT_PROD_EXPR:
      {
	arg1 = type_convert(arg1, arg1_type, arg3_type, bb);
	arg2 = type_convert(arg2, arg2_type, arg3_type, bb);
	Instruction *inst = bb->build_inst(Op::MUL, arg1, arg2);
	return bb->build_inst(Op::ADD, inst, arg3);
      }
    default:
      throw Not_implemented("process_ternary: "s + get_tree_code_name(code));
    }
}

Instruction *Converter::process_ternary_vec(enum tree_code code, Instruction *arg1, Instruction *arg2, Instruction *arg3, tree lhs_type, tree arg1_type, tree arg2_type, tree arg3_type, Basic_block *bb)
{
  assert(VECTOR_TYPE_P(lhs_type));
  assert(VECTOR_TYPE_P(arg1_type));
  assert(VECTOR_TYPE_P(arg2_type));
  assert(VECTOR_TYPE_P(arg3_type));

  tree arg1_elem_type = TREE_TYPE(arg1_type);
  uint32_t arg1_elem_bitsize = bitsize_for_type(arg1_elem_type);
  tree arg2_elem_type = TREE_TYPE(arg2_type);
  uint32_t arg2_elem_bitsize = bitsize_for_type(arg2_elem_type);
  tree arg3_elem_type = TREE_TYPE(arg3_type);
  uint32_t arg3_elem_bitsize = bitsize_for_type(arg3_elem_type);

  uint32_t nof_elt3 = bitsize_for_type(arg3_type) / arg3_elem_bitsize;
  uint32_t nof_elt = bitsize_for_type(arg1_type) / arg1_elem_bitsize;
  Instruction *res = nullptr;
  for (uint64_t i = 0; i < nof_elt; i++)
    {
      Instruction *a1 = extract_vec_elem(bb, arg1, arg1_elem_bitsize, i);
      Instruction *a2 = extract_vec_elem(bb, arg2, arg2_elem_bitsize, i);
      // Instructions such as SAD_EXPR has fewer elements in the arg3,
      // and it iterates multiple times and updates that.
      uint32_t i3 = i % nof_elt3;
      if (!i3 && res)
	{
	  arg3 = res;
	  res = nullptr;
	}
      Instruction *a3 = extract_vec_elem(bb, arg3, arg3_elem_bitsize, i3);
      Instruction *inst = process_ternary(code, a1, a2, a3, arg1_elem_type,
					  arg2_elem_type, arg3_elem_type, bb);
      if (res)
	res = bb->build_inst(Op::CONCAT, inst, res);
      else
	res = inst;
    }
  return res;
}

std::pair<Instruction *, Instruction *> Converter::process_vec_cond(Instruction *arg1, Instruction *arg2, Instruction *arg2_undef, Instruction *arg3, Instruction *arg3_undef, tree arg1_type, tree arg2_type, Basic_block *bb)
{
  assert(VECTOR_TYPE_P(arg1_type));
  assert(VECTOR_TYPE_P(arg2_type));
  assert(arg2->bitsize == arg3->bitsize);

  if (arg2_undef || arg3_undef)
    {
      if (!arg2_undef)
	arg2_undef = bb->value_inst(0, arg2->bitsize);
      if (!arg3_undef)
	arg3_undef = bb->value_inst(0, arg3->bitsize);
    }

  tree arg1_elem_type = TREE_TYPE(arg1_type);
  assert(TREE_CODE(arg1_elem_type) == BOOLEAN_TYPE);
  tree arg2_elem_type = TREE_TYPE(arg2_type);

  uint32_t elem_bitsize1 = bitsize_for_type(arg1_elem_type);
  uint32_t elem_bitsize2 = bitsize_for_type(arg2_elem_type);

  Instruction *res = nullptr;
  Instruction *res_undef = nullptr;
  uint32_t nof_elt = bitsize_for_type(arg1_type) / elem_bitsize1;
  for (uint64_t i = 0; i < nof_elt; i++)
    {
      Instruction *a1 = extract_vec_elem(bb, arg1, elem_bitsize1, i);
      if (TYPE_PRECISION(arg1_elem_type) != 1)
	a1 = bb->build_extract_bit(a1, 0);
      Instruction *a2 = extract_vec_elem(bb, arg2, elem_bitsize2, i);
      Instruction *a3 = extract_vec_elem(bb, arg3, elem_bitsize2, i);

      if (arg2_undef)
	{
	  Instruction *a2_undef =
	    extract_vec_elem(bb, arg2_undef, elem_bitsize2, i);
	  Instruction *a3_undef =
	    extract_vec_elem(bb, arg3_undef, elem_bitsize2, i);
	  Instruction *undef =
	    bb->build_inst(Op::ITE, a1, a2_undef, a3_undef);

	  if (res_undef)
	    res_undef = bb->build_inst(Op::CONCAT, undef, res_undef);
	  else
	    res_undef = undef;
	}

      Instruction *inst = bb->build_inst(Op::ITE, a1, a2, a3);
      if (res)
	res = bb->build_inst(Op::CONCAT, inst, res);
      else
	res = inst;
    }

  return {res, res_undef};
}

std::pair<Instruction *, Instruction *> Converter::process_vec_perm_expr(gimple *stmt, Basic_block *bb)
{
  auto [arg1, arg1_undef] = tree2inst(bb, gimple_assign_rhs1(stmt));
  auto [arg2, arg2_undef] = tree2inst(bb, gimple_assign_rhs2(stmt));
  Instruction *arg3 = tree2inst_undefcheck(bb, gimple_assign_rhs3(stmt));
  assert(arg1->bitsize == arg2->bitsize);
  tree arg1_type = TREE_TYPE(gimple_assign_rhs1(stmt));
  tree arg1_elem_type = TREE_TYPE(arg1_type);
  tree arg3_type = TREE_TYPE(gimple_assign_rhs3(stmt));
  tree arg3_elem_type = TREE_TYPE(arg3_type);
  uint32_t elem_bitsize1 = bitsize_for_type(arg1_elem_type);
  uint32_t elem_bitsize3 = bitsize_for_type(arg3_elem_type);
  uint32_t nof_elt1 = bitsize_for_type(arg1_type) / elem_bitsize1;
  uint32_t nof_elt3 = bitsize_for_type(arg3_type) / elem_bitsize3;

  if (arg1_undef || arg2_undef)
    {
      if (!arg1_undef)
	arg1_undef = bb->value_inst(0, arg1->bitsize);
      if (!arg2_undef)
	arg2_undef = bb->value_inst(0, arg2->bitsize);
    }

  Instruction *mask1 = bb->value_inst(nof_elt1 * 2 - 1, elem_bitsize3);
  Instruction *mask2 = bb->value_inst(nof_elt1 - 1, elem_bitsize3);
  Instruction *nof_elt_inst = bb->value_inst(nof_elt1, elem_bitsize3);
  Instruction *res = nullptr;
  Instruction *res_undef = nullptr;
  for (uint64_t i = 0; i < nof_elt3; i++)
    {
      Instruction *idx1 = extract_vec_elem(bb, arg3, elem_bitsize3, i);
      idx1 = bb->build_inst(Op::AND, idx1, mask1);
      Instruction *idx2 = bb->build_inst(Op::AND, idx1, mask2);
      Instruction *cmp = bb->build_inst(Op::ULT, idx1,  nof_elt_inst);
      Instruction *elt1 = extract_elem(bb, arg1, elem_bitsize1, idx2);
      Instruction *elt2 = extract_elem(bb, arg2, elem_bitsize1, idx2);
      Instruction *inst = bb->build_inst(Op::ITE, cmp, elt1, elt2);
      if (res)
	res = bb->build_inst(Op::CONCAT, inst, res);
      else
	res = inst;

      if (arg1_undef)
	{
	  Instruction *undef1 =
	    extract_elem(bb, arg1_undef, elem_bitsize1, idx2);
	  Instruction *undef2 =
	    extract_elem(bb, arg2_undef, elem_bitsize1, idx2);
	  Instruction *undef = bb->build_inst(Op::ITE, cmp, undef1, undef2);
	  if (res_undef)
	    res_undef = bb->build_inst(Op::CONCAT, undef, res_undef);
	  else
	    res_undef = undef;
	}
    }
  return {res, res_undef};
}

std::pair<Instruction *, Instruction *> Converter::vector_constructor(Basic_block *bb, tree expr)
{
  assert(TREE_CODE(expr) == CONSTRUCTOR);
  assert(VECTOR_TYPE_P(TREE_TYPE(expr)));
  unsigned HOST_WIDE_INT idx;
  tree value;
  uint32_t vector_size = bytesize_for_type(TREE_TYPE(expr)) * 8;
  Instruction *res = nullptr;
  Instruction *undef = nullptr;
  bool any_elem_has_undef = false;
  // Note: The contstuctor elements may have different sizes. For example,
  // we may create a vector by concatenating a scalar with a vector.
  FOR_EACH_CONSTRUCTOR_VALUE(CONSTRUCTOR_ELTS(expr), idx, value)
    {
      auto [elem, elem_undef] = tree2inst(bb, value);
      if (elem_undef)
	{
	  any_elem_has_undef = true;
	}
      else
	elem_undef = bb->value_inst(0, elem->bitsize);
      if (res)
	{
	  res = bb->build_inst(Op::CONCAT, elem, res);
	  undef = bb->build_inst(Op::CONCAT, elem_undef, undef);
	}
      else
	{
	  assert(idx == 0);
	  res = elem;
	  undef = elem_undef;
	}
    }
  assert(res);
  assert(res->bitsize <= vector_size);
  if (CONSTRUCTOR_NO_CLEARING(expr))
    throw Not_implemented("vector_constructor: CONSTRUCTOR_NO_CLEARING");
  if (res->bitsize != vector_size)
    {
      Instruction *zero = bb->value_inst(0, vector_size - res->bitsize);
      res = bb->build_inst(Op::CONCAT, zero, res);
      undef = bb->build_inst(Op::CONCAT, zero, undef);
    }
  if (!any_elem_has_undef)
    {
      // No element had undef information, so `undef` only consists of the
      // zero values we creates. Change it to the `nullptr` so that later
      // code does not need to add UB comparisions each place the result
      // is used.
      undef = nullptr;
    }
  return {res, undef};
}

void Converter::process_constructor(tree lhs, tree rhs, Basic_block *bb)
{
  Addr dest_addr = process_address(bb, lhs);
  assert(!dest_addr.bitoffset);
  Instruction *dest = dest_addr.ptr;
  Instruction *mem_id = bb->build_extract_id(dest);

  if (TREE_CLOBBER_P(rhs) && CLOBBER_KIND(rhs) == CLOBBER_EOL)
    {
      bb->build_inst(Op::FREE, mem_id);
      return;
    }

  assert(!CONSTRUCTOR_NO_CLEARING(rhs));
  Instruction *ptr = dest;
  Instruction *one = bb->value_inst(1, ptr->bitsize);
  uint64_t size = bytesize_for_type(TREE_TYPE(rhs));
  if (size > MAX_MEMORY_UNROLL_LIMIT)
    throw Not_implemented("process_constructor: too large constructor");
  store_ub_check(bb, ptr, size);

  if (TREE_CLOBBER_P(rhs))
    make_uninit(bb, ptr, size);
  else
    {
      Instruction *zero = bb->value_inst(0, 8);
      Instruction *memory_flag = bb->value_inst(1, 1);
      for (uint64_t i = 0; i < size; i++)
	{
	  uint8_t padding = padding_at_offset(TREE_TYPE(rhs), i);
	  Instruction *undef = bb->value_inst(padding, 8);
	  bb->build_inst(Op::STORE, ptr, zero);
	  bb->build_inst(Op::SET_MEM_UNDEF, ptr, undef);
	  bb->build_inst(Op::SET_MEM_FLAG, ptr, memory_flag);
	  ptr = bb->build_inst(Op::ADD, ptr, one);
	}
    }

  assert(!CONSTRUCTOR_NELTS(rhs));
}

void Converter::process_gimple_assign(gimple *stmt, Basic_block *bb)
{
  tree lhs = gimple_assign_lhs(stmt);
  check_type(TREE_TYPE(lhs));
  enum tree_code code = gimple_assign_rhs_code(stmt);

  if (TREE_CODE(lhs) != SSA_NAME)
    {
      assert(get_gimple_rhs_class(code) == GIMPLE_SINGLE_RHS);
      tree rhs = gimple_assign_rhs1(stmt);
      if (TREE_CODE(rhs) == CONSTRUCTOR)
	process_constructor(lhs, rhs, bb);
      else
	process_store(lhs, rhs, bb);
      return;
    }

  tree rhs1 = gimple_assign_rhs1(stmt);
  check_type(TREE_TYPE(rhs1));
  Instruction *inst;
  Instruction *undef = nullptr;
  switch (get_gimple_rhs_class(code))
    {
    case GIMPLE_TERNARY_RHS:
      {
	if (code == SAD_EXPR || code == DOT_PROD_EXPR)
	  {
	    Instruction *arg1 =
	      tree2inst_undefcheck(bb, gimple_assign_rhs1(stmt));
	    Instruction *arg2 =
	      tree2inst_undefcheck(bb, gimple_assign_rhs2(stmt));
	    Instruction *arg3 =
	      tree2inst_undefcheck(bb, gimple_assign_rhs3(stmt));
	    tree lhs_type = TREE_TYPE(gimple_assign_lhs(stmt));
	    tree arg1_type = TREE_TYPE(gimple_assign_rhs1(stmt));
	    tree arg2_type = TREE_TYPE(gimple_assign_rhs2(stmt));
	    tree arg3_type = TREE_TYPE(gimple_assign_rhs3(stmt));
	    if (VECTOR_TYPE_P(lhs_type))
	      inst = process_ternary_vec(code, arg1, arg2, arg3, lhs_type, arg1_type, arg2_type, arg3_type, bb);
	    else
	      inst = process_ternary(code, arg1, arg2, arg3, arg1_type, arg2_type, arg3_type, bb);
	  }
	else if (code == VEC_PERM_EXPR)
	  std::tie(inst, undef) = process_vec_perm_expr(stmt, bb);
	else if (code == VEC_COND_EXPR)
	  {
	    Instruction *arg1 =
	      tree2inst_undefcheck(bb, gimple_assign_rhs1(stmt));
	    auto [arg2, arg2_undef] = tree2inst(bb, gimple_assign_rhs2(stmt));
	    auto [arg3, arg3_undef] = tree2inst(bb, gimple_assign_rhs3(stmt));
	    tree arg1_type = TREE_TYPE(gimple_assign_rhs1(stmt));
	    tree arg2_type = TREE_TYPE(gimple_assign_rhs2(stmt));
	    std::tie(inst, undef) =
	      process_vec_cond(arg1, arg2, arg2_undef, arg3, arg3_undef,
			       arg1_type, arg2_type, bb);
	  }
	else if (code == COND_EXPR)
	  {
	    tree rhs1 = gimple_assign_rhs1(stmt);
	    assert(TREE_CODE(TREE_TYPE(rhs1)) == BOOLEAN_TYPE);
	    Instruction *arg1 = tree2inst_undefcheck(bb, rhs1);
	    if (TYPE_PRECISION(TREE_TYPE(rhs1)) != 1)
	      arg1 = bb->build_extract_bit(arg1, 0);
	    auto [arg2, arg2_undef] = tree2inst(bb, gimple_assign_rhs2(stmt));
	    auto [arg3, arg3_undef] = tree2inst(bb, gimple_assign_rhs3(stmt));
	    if (arg2_undef || arg3_undef)
	      {
		if (!arg2_undef)
		  arg2_undef = bb->value_inst(0, arg2->bitsize);
		if (!arg3_undef)
		  arg3_undef = bb->value_inst(0, arg3->bitsize);
		undef =
		  bb->build_inst(Op::ITE, arg1, arg2_undef, arg3_undef);
	      }
	    inst = bb->build_inst(Op::ITE, arg1, arg2, arg3);
	  }
	else if (code == BIT_INSERT_EXPR)
	  {
	    auto [arg1, arg1_undef] = tree2inst(bb, gimple_assign_rhs1(stmt));
	    tree arg2_expr = gimple_assign_rhs2(stmt);
	    auto [arg2, arg2_undef] = tree2inst(bb, arg2_expr);
	    bool has_undef = arg1_undef || arg2_undef;
	    if (has_undef)
	      {
		if (!arg1_undef)
		  arg1_undef = bb->value_inst(0, arg1->bitsize);
		if (!arg2_undef)
		  arg2_undef = bb->value_inst(0, arg2->bitsize);
	      }
	    uint64_t bit_pos = get_int_cst_val(gimple_assign_rhs3(stmt));
	    if (bit_pos > 0)
	      {
		Instruction *extract = bb->build_trunc(arg1, bit_pos);
		inst = bb->build_inst(Op::CONCAT, arg2, extract);
		if (has_undef)
		  {
		    Instruction *extract_undef =
		      bb->build_trunc(arg1_undef, bit_pos);
		    undef =
		      bb->build_inst(Op::CONCAT, arg2_undef, extract_undef);
		  }
	      }
	    else
	      {
		inst = arg2;
		if (has_undef)
		  undef = arg2_undef;
	      }
	    if (bit_pos + arg2->bitsize != arg1->bitsize)
	      {
		Instruction *high = bb->value_inst(arg1->bitsize - 1, 32);
		Instruction *low = bb->value_inst(bit_pos + arg2->bitsize, 32);
		Instruction *extract =
		  bb->build_inst(Op::EXTRACT, arg1, high, low);
		inst = bb->build_inst(Op::CONCAT, extract, inst);
		if (has_undef)
		  {
		    Instruction *extract_undef =
		      bb->build_inst(Op::EXTRACT, arg1_undef, high, low);
		    undef = bb->build_inst(Op::CONCAT, extract_undef, undef);
		  }
	      }
	  }
	else
	  throw Not_implemented("GIMPLE_TERNARY_RHS: "s + get_tree_code_name(code));
      }
      break;
    case GIMPLE_BINARY_RHS:
      {
	tree lhs_type = TREE_TYPE(gimple_assign_lhs(stmt));
	tree rhs1 = gimple_assign_rhs1(stmt);
	tree rhs2 = gimple_assign_rhs2(stmt);
	tree arg1_type = TREE_TYPE(rhs1);
	tree arg2_type = TREE_TYPE(rhs2);
	if (TREE_CODE(lhs_type) == COMPLEX_TYPE && code == COMPLEX_EXPR)
	  {
	    auto [arg1, arg1_undef] = tree2inst(bb, rhs1);
	    auto [arg2, arg2_undef] = tree2inst(bb, rhs2);
	    arg1 = to_mem_repr(bb, arg1, TREE_TYPE(rhs1));
	    arg2 = to_mem_repr(bb, arg2, TREE_TYPE(rhs2));
	    inst = bb->build_inst(Op::CONCAT, arg2, arg1);
	    if (arg1_undef || arg2_undef)
	      {
		if (!arg1_undef)
		  arg1_undef = bb->value_inst(0, arg1->bitsize);
		if (!arg2_undef)
		  arg2_undef = bb->value_inst(0, arg2->bitsize);
		undef =
		  bb->build_inst(Op::CONCAT, arg2_undef, arg1_undef);
	      }
	  }
	else
	  {
	    if (TREE_CODE(lhs_type) == COMPLEX_TYPE)
	      {
		Instruction *arg1 = tree2inst_undefcheck(bb, rhs1);
		Instruction *arg2 = tree2inst_undefcheck(bb, rhs2);
		inst = process_binary_complex(code, arg1, arg2, lhs_type, bb);
	      }
	    else if (TREE_CODE(arg1_type) == COMPLEX_TYPE)
	      {
		Instruction *arg1 = tree2inst_undefcheck(bb, rhs1);
		Instruction *arg2 = tree2inst_undefcheck(bb, rhs2);
		inst = process_binary_complex_cmp(code, arg1, arg2, lhs_type,
						  arg1_type, bb);
	      }
	    else if (VECTOR_TYPE_P(lhs_type))
	      {
		auto [arg1, arg1_undef] = tree2inst(bb, rhs1);
		auto [arg2, arg2_undef] = tree2inst(bb, rhs2);
		std::tie(inst, undef) =
		  process_binary_vec(code, arg1, arg1_undef, arg2,
				     arg2_undef, lhs_type, arg1_type,
				     arg2_type, bb);
	      }
	    else
	      {
		auto [arg1, arg1_undef] = tree2inst(bb, rhs1);
		auto [arg2, arg2_undef] = tree2inst(bb, rhs2);
		std::tie(inst, undef) =
		  process_binary_scalar(code, arg1, arg1_undef, arg2,
					arg2_undef, lhs_type,
					arg1_type, arg2_type, bb);
	      }
	  }
      }
      break;
    case GIMPLE_UNARY_RHS:
      {
	tree rhs1 = gimple_assign_rhs1(stmt);
	tree lhs_type = TREE_TYPE(gimple_assign_lhs(stmt));
	tree arg1_type = TREE_TYPE(rhs1);
	if (TREE_CODE(lhs_type) == COMPLEX_TYPE
	    || TREE_CODE(arg1_type) == COMPLEX_TYPE)
	  {
	    Instruction *arg1 = tree2inst_undefcheck(bb, rhs1);
	    inst = process_unary_complex(code, arg1, lhs_type, bb);
	  }
	else if (VECTOR_TYPE_P(lhs_type))
	  {
	    auto [arg1, arg1_undef] = tree2inst(bb, rhs1);
	    tree lhs_elem_type = TREE_TYPE(lhs_type);
	    tree arg1_elem_type = TREE_TYPE(arg1_type);
	    std::tie(inst, undef) =
	      process_unary_vec(code, arg1, arg1_undef, lhs_elem_type,
				arg1_elem_type, bb);
	  }
	else
	  {
	    auto [arg1, arg1_undef] = tree2inst(bb, rhs1);
	    std::tie(inst, undef) =
	      process_unary_scalar(code, arg1, arg1_undef, lhs_type,
				   arg1_type, bb);
	  }
      }
      break;
    case GIMPLE_SINGLE_RHS:
      std::tie(inst, undef) = tree2inst(bb, gimple_assign_rhs1(stmt));
      break;
    default:
      throw Not_implemented("unknown get_gimple_rhs_class");
    }

  constrain_range(bb, lhs, inst, undef);

  assert(TREE_CODE(lhs) == SSA_NAME);
  tree2instruction[lhs] = inst;
  if (undef)
    tree2undef[lhs] = undef;
}

void Converter::process_gimple_asm(gimple *stmt)
{
  gasm *asm_stmt = as_a<gasm *>(stmt);
  const char *p = gimple_asm_string(asm_stmt);

  // We can ignore asm having an empty string (as they only constrain
  // optimizations in ways that does not affect us).
  while (*p)
    {
      if (!ISSPACE(*p++))
	throw Not_implemented("process_function: gimple_asm");
    }

  // Empty asm goto gives us problems with GIMPLE BBs with the wrong
  // number of EGHE_COUNT preds/succs. This is easy to fix, but does
  // not give us any benefit until we have real asm handling.
  if (gimple_asm_nlabels(asm_stmt))
    throw Not_implemented("process_function: gimple_asm");
}

void Converter::process_gimple_call_builtin(gimple *stmt, Basic_block *bb)
{
  tree fn = gimple_call_fndecl(stmt);
  const std::string name = fndecl_name(fn);

  if (name == "__builtin_assume_aligned"s)
    {
      Instruction *arg1 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *arg2 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 1));
      assert(arg1->bitsize == arg2->bitsize);
      Instruction *one = bb->value_inst(1, arg2->bitsize);
      Instruction *mask = bb->build_inst(Op::SUB, arg2, one);
      Instruction *val = bb->build_inst(Op::AND, arg1, mask);
      Instruction *zero = bb->value_inst(0, val->bitsize);
      Instruction *cond = bb->build_inst(Op::NE, val, zero);
      bb->build_inst(Op::UB, cond);
      tree lhs = gimple_call_lhs(stmt);
      if (lhs)
	{
	  constrain_range(bb, lhs, arg1);
	  tree2instruction[lhs] = arg1;
	}
      return;
    }

  if (name == "__builtin_bswap16"s ||
      name == "__builtin_bswap32"s ||
      name == "__builtin_bswap64"s ||
      name == "__builtin_bswap128"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      auto [arg, arg_undef] = tree2inst(bb, gimple_call_arg(stmt, 0));
      // Determine the width from lhs as bswap16 has 32-bit arg.
      int bitwidth = TYPE_PRECISION(TREE_TYPE(lhs));
      Instruction *inst = bb->build_trunc(arg, 8);
      Instruction *inst_undef = nullptr;
      if (arg_undef)
	inst_undef = bb->build_trunc(arg_undef, 8);
      for (int i = 8; i < bitwidth; i += 8)
	{
	  Instruction *high = bb->value_inst(i + 7, 32);
	  Instruction *low = bb->value_inst(i, 32);
	  Instruction *byte = bb->build_inst(Op::EXTRACT, arg, high, low);
	  inst = bb->build_inst(Op::CONCAT, inst, byte);
	  if (arg_undef)
	    {
	      Instruction *byte_undef =
		bb->build_inst(Op::EXTRACT, arg_undef, high, low);
	      inst_undef = bb->build_inst(Op::CONCAT, inst_undef, byte_undef);
	    }
	}
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      if (inst_undef)
	tree2undef[lhs] = inst_undef;
      return;
    }

  if (name == "__builtin_clrsb"s ||
      name == "__builtin_clrsbl"s ||
      name == "__builtin_clrsbll"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      assert(arg->bitsize > 1);
      int bitsize = bitsize_for_type(TREE_TYPE(lhs));
      Instruction *signbit = bb->build_extract_bit(arg, arg->bitsize - 1);
      Instruction *inst = bb->value_inst(arg->bitsize - 1, bitsize);
      for (unsigned i = 0; i < arg->bitsize - 1; i++)
	{
	  Instruction *bit = bb->build_extract_bit(arg, i);
	  Instruction *cmp = bb->build_inst(Op::NE, bit, signbit);
	  Instruction *val = bb->value_inst(arg->bitsize - i - 2, bitsize);
	  inst = bb->build_inst(Op::ITE, cmp, val, inst);
	}
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "__builtin_clz"s ||
      name == "__builtin_clzl"s ||
      name == "__builtin_clzll"s)
    {
      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *zero = bb->value_inst(0, arg->bitsize);
      Instruction *ub = bb->build_inst(Op::EQ, arg, zero);
      bb->build_inst(Op::UB, ub);
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      int bitsize = bitsize_for_type(TREE_TYPE(lhs));
      Instruction *inst = bb->value_inst(arg->bitsize, bitsize);
      for (unsigned i = 0; i < arg->bitsize; i++)
	{
	  Instruction *bit = bb->build_extract_bit(arg, i);
	  Instruction *val = bb->value_inst(arg->bitsize - i - 1, bitsize);
	  inst = bb->build_inst(Op::ITE, bit, val, inst);
	}
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "__builtin_copysign"s ||
      name == "__builtin_copysignf"s ||
      name == "__builtin_copysignl"s ||
      name == "__builtin_copysignf16"s ||
      name == "__builtin_copysignf32"s ||
      name == "__builtin_copysignf32x"s ||
      name == "__builtin_copysignf64"s ||
      name == "__builtin_copysignf128"s ||
      name == "copysign"s ||
      name == "copysignf"s)
    {
      Instruction *arg1 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *arg2 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 1));
      Instruction *signbit = bb->build_extract_bit(arg2, arg2->bitsize - 1);
      arg1 = bb->build_trunc(arg1, arg1->bitsize - 1);
      arg1 = bb->build_inst(Op::CONCAT, signbit, arg1);
      Instruction *cond = bb->build_inst(Op::IS_NONCANONICAL_NAN, arg1);
      bb->build_inst(Op::UB, cond);
      tree lhs = gimple_call_lhs(stmt);
      if (lhs)
	{
	  constrain_range(bb, lhs, arg1);
	  tree2instruction[lhs] = arg1;
	}
      return;
    }

  if (name == "__builtin_ctz"s ||
      name == "__builtin_ctzl"s ||
      name == "__builtin_ctzll"s)
    {
      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *zero = bb->value_inst(0, arg->bitsize);
      Instruction *ub = bb->build_inst(Op::EQ, arg, zero);
      bb->build_inst(Op::UB, ub);
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      int bitsize = bitsize_for_type(TREE_TYPE(lhs));
      Instruction *inst = bb->value_inst(arg->bitsize, bitsize);
      for (int i = arg->bitsize - 1; i >= 0; i--)
	{
	  Instruction *bit = bb->build_extract_bit(arg, i);
	  Instruction *val = bb->value_inst(i, bitsize);
	  inst = bb->build_inst(Op::ITE, bit, val, inst);
	}
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "__builtin_expect"s ||
      name == "__builtin_expect_with_probability"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      constrain_range(bb, lhs, arg);
      tree2instruction[lhs] = arg;
      return;
    }

  if (name == "__builtin_fmax"s ||
      name == "__builtin_fmaxf"s ||
      name == "__builtin_fmaxl"s ||
      name == "fmax"s ||
      name == "fmaxf"s ||
      name == "fmaxl"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *arg1 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *arg2 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 1));
      Instruction *is_nan = bb->build_inst(Op::IS_NAN, arg2);
      Instruction *cmp = bb->build_inst(Op::FGT, arg1, arg2);
      Instruction *max1 = bb->build_inst(Op::ITE, cmp, arg1, arg2);
      Instruction *max2 = bb->build_inst(Op::ITE, is_nan, arg1, max1);

      // 0.0 and -0.0 is equal as floating point values, and fmax(0.0, -0.0)
      // may return eiter of them. But we treat them as 0.0 > -0.0 here,
      // otherwise we will report miscompilations when GCC switch the order
      // of the arguments.
      Instruction *zero = bb->value_inst(0, arg1->bitsize);
      Instruction *is_zero1 = bb->build_inst(Op::FEQ, arg1, zero);
      Instruction *is_zero2 = bb->build_inst(Op::FEQ, arg2, zero);
      Instruction *is_zero = bb->build_inst(Op::AND, is_zero1, is_zero2);
      Instruction *cmp2 = bb->build_inst(Op::SGT, arg1, arg2);
      Instruction *max3 = bb->build_inst(Op::ITE, cmp2, arg1, arg2);
      tree2instruction[lhs] = bb->build_inst(Op::ITE, is_zero, max3, max2);
      return;
    }

  if (name == "__builtin_fmin"s ||
      name == "__builtin_fminf"s ||
      name == "__builtin_fminl"s ||
      name == "fmin"s ||
      name == "fminf"s ||
      name == "fminl"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *arg1 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *arg2 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 1));
      Instruction *is_nan = bb->build_inst(Op::IS_NAN, arg2);
      Instruction *cmp = bb->build_inst(Op::FLT, arg1, arg2);
      Instruction *min1 = bb->build_inst(Op::ITE, cmp, arg1, arg2);
      Instruction *min2 = bb->build_inst(Op::ITE, is_nan, arg1, min1);

      // 0.0 and -0.0 is equal as floating point values, and fmin(0.0, -0.0)
      // may return eiter of them. But we treat them as 0.0 > -0.0 here,
      // otherwise we will report miscompilations when GCC switch the order
      // of the arguments.
      Instruction *zero = bb->value_inst(0, arg1->bitsize);
      Instruction *is_zero1 = bb->build_inst(Op::FEQ, arg1, zero);
      Instruction *is_zero2 = bb->build_inst(Op::FEQ, arg2, zero);
      Instruction *is_zero = bb->build_inst(Op::AND, is_zero1, is_zero2);
      Instruction *cmp2 = bb->build_inst(Op::SLT, arg1, arg2);
      Instruction *min3 = bb->build_inst(Op::ITE, cmp2, arg1, arg2);
      tree2instruction[lhs] = bb->build_inst(Op::ITE, is_zero, min3, min2);
      return;
    }

  if (name == "__builtin_memcpy"s ||
      name == "memcpy"s)
    {
      if (TREE_CODE(gimple_call_arg(stmt, 2)) != INTEGER_CST)
	throw Not_implemented("non-constant memcpy size");
      Instruction *dest_ptr =
	tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *src_ptr =
	tree2inst_undefcheck(bb, gimple_call_arg(stmt, 1));
      unsigned __int128 size = get_int_cst_val(gimple_call_arg(stmt, 2));
      if (size > MAX_MEMORY_UNROLL_LIMIT)
	throw Not_implemented("too large memcpy");

      store_ub_check(bb, dest_ptr, size);
      load_ub_check(bb, src_ptr, size);

      tree lhs = gimple_call_lhs(stmt);
      if (lhs)
	{
	  constrain_range(bb, lhs, dest_ptr);
	  tree2instruction[lhs] = dest_ptr;
	}

      Instruction *one = bb->value_inst(1, src_ptr->bitsize);
      for (size_t i = 0; i < size; i++)
	{
	  Instruction *byte = bb->build_inst(Op::LOAD, src_ptr);
	  bb->build_inst(Op::STORE, dest_ptr, byte);

	  Instruction *mem_flag = bb->build_inst(Op::GET_MEM_FLAG, src_ptr);
	  bb->build_inst(Op::SET_MEM_FLAG, dest_ptr, mem_flag);

	  Instruction *undef = bb->build_inst(Op::GET_MEM_UNDEF, src_ptr);
	  bb->build_inst(Op::SET_MEM_UNDEF, dest_ptr, undef);

	  src_ptr = bb->build_inst(Op::ADD, src_ptr, one);
	  dest_ptr = bb->build_inst(Op::ADD, dest_ptr, one);
	}
      return;
    }

  if (name == "__builtin_nan"s ||
      name == "__builtin_nanf"s ||
      name == "__builtin_nanl"s ||
      name == "nan"s ||
      name == "nanf"s ||
      name == "nanl"s)
    {
      // TODO: Implement the argument setting NaN payload when support for
      // noncanonical NaNs is implemented in the SMT solvers.
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;

      Instruction *bs = bb->value_inst(bitsize_for_type(TREE_TYPE(lhs)), 32);
      tree2instruction[lhs] = bb->build_inst(Op::NAN, bs);
      return;
    }

  if (name == "__builtin_memset"s ||
      name == "memset"s)
    {
      if (TREE_CODE(gimple_call_arg(stmt, 2)) != INTEGER_CST)
	throw Not_implemented("non-constant memset size");
      Instruction *ptr = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *value = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 1));
      unsigned __int128 size = get_int_cst_val(gimple_call_arg(stmt, 2));
      if (size > MAX_MEMORY_UNROLL_LIMIT)
	throw Not_implemented("too large memset");

      store_ub_check(bb, ptr, size);

      tree lhs = gimple_call_lhs(stmt);
      if (lhs)
	{
	  constrain_range(bb, lhs, ptr);
	  tree2instruction[lhs] = ptr;
	}

      assert(value->bitsize >= 8);
      if (value->bitsize > 8)
	value = bb->build_trunc(value, 8);
      Instruction *one = bb->value_inst(1, ptr->bitsize);
      Instruction *mem_flag = bb->value_inst(1, 1);
      Instruction *undef = bb->value_inst(0, 8);
      for (size_t i = 0; i < size; i++)
	{
	  bb->build_inst(Op::STORE, ptr, value);
	  bb->build_inst(Op::SET_MEM_FLAG, ptr, mem_flag);
	  bb->build_inst(Op::SET_MEM_UNDEF, ptr, undef);
	  ptr = bb->build_inst(Op::ADD, ptr, one);
	}
      return;
    }

  if (name == "__builtin_parity"s ||
      name == "__builtin_parityl"s ||
      name == "__builtin_parityll"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      int bitwidth = arg->bitsize;
      Instruction *inst = bb->build_extract_bit(arg, 0);
      for (int i = 1; i < bitwidth; i++)
	{
	  Instruction *bit = bb->build_extract_bit(arg, i);
	  inst = bb->build_inst(Op::XOR, inst, bit);
	}
      bitwidth = TYPE_PRECISION(TREE_TYPE(lhs));
      Instruction *bitwidth_inst = bb->value_inst(bitwidth, 32);
      inst = bb->build_inst(Op::ZEXT, inst, bitwidth_inst);
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "__builtin_popcount"s ||
      name == "__builtin_popcountl"s ||
      name == "__builtin_popcountll"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      int bitwidth = arg->bitsize;
      Instruction *eight = bb->value_inst(8, 32);
      Instruction *bit = bb->build_extract_bit(arg, 0);
      Instruction *res = bb->build_inst(Op::ZEXT, bit, eight);
      for (int i = 1; i < bitwidth; i++)
	{
	  bit = bb->build_extract_bit(arg, i);
	  Instruction *ext = bb->build_inst(Op::ZEXT, bit, eight);
	  res = bb->build_inst(Op::ADD, res, ext);
	}
      int lhs_bitwidth = TYPE_PRECISION(TREE_TYPE(lhs));
      Instruction *lhs_bitwidth_inst = bb->value_inst(lhs_bitwidth, 32);
      res = bb->build_inst(Op::ZEXT, res, lhs_bitwidth_inst);
      constrain_range(bb, lhs, res);
      tree2instruction[lhs] = res;
      return;
    }

  if (name == "__builtin_signbit"s ||
      name == "__builtin_signbitf"s ||
      name == "signbit"s ||
      name == "signbitf"s)
    {
      Instruction *arg1 = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      Instruction *cond = bb->build_inst(Op::IS_NONCANONICAL_NAN, arg1);
      bb->build_inst(Op::UB, cond);
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      Instruction *signbit = bb->build_extract_bit(arg1, arg1->bitsize - 1);
      uint32_t bitsize = bitsize_for_type(TREE_TYPE(lhs));
      Instruction *lhs_bitsize_inst = bb->value_inst(bitsize, 32);
      Instruction *inst = bb->build_inst(Op::ZEXT, signbit, lhs_bitsize_inst);
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "__builtin_unreachable" ||
      name == "__builtin_unreachable trap")
    {
      bb->build_inst(Op::UB, bb->value_inst(1, 1));
      return;
    }

  if (name == "__builtin_trap")
    {
      // TODO: Some passes add __builtin_trap for cases that are UB (so that
      // the program terminates instead of continuing in a random state).
      // We threat these as UB for now, but they should arguably be handled
      // in a special way to verify that we actually are termininating.
      bb->build_inst(Op::UB, bb->value_inst(1, 1));
      return;
    }

  throw Not_implemented("process_gimple_call_builtin: "s + name);
}

void Converter::process_gimple_call_internal(gimple *stmt, Basic_block *bb)
{
  const std::string name = internal_fn_name(gimple_call_internal_fn(stmt));

  if (name == "FALLTHROUGH"s)
    return;

  if (name == "ADD_OVERFLOW"s
      || name == "SUB_OVERFLOW"s
      || name == "MUL_OVERFLOW"s)
    {
      tree arg1_expr = gimple_call_arg(stmt, 0);
      tree arg1_type = TREE_TYPE(arg1_expr);
      tree arg2_expr = gimple_call_arg(stmt, 1);
      tree arg2_type = TREE_TYPE(arg2_expr);
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      tree lhs_elem_type = TREE_TYPE(TREE_TYPE(lhs));
      Instruction *arg1 = tree2inst_undefcheck(bb, arg1_expr);
      Instruction *arg2 = tree2inst_undefcheck(bb, arg2_expr);
      unsigned lhs_elem_bitsize = bitsize_for_type(lhs_elem_type);
      unsigned bitsize;
      if (name == "MUL_OVERFLOW"s)
	bitsize = 1 + std::max(arg1->bitsize + arg2->bitsize, lhs_elem_bitsize);
      else
	{
	  bitsize = 1 + std::max(arg1->bitsize, arg2->bitsize);
	  bitsize = 1 + std::max(bitsize, lhs_elem_bitsize);
	}
      Instruction *bitsize_inst = bb->value_inst(bitsize, 32);
      if (TYPE_UNSIGNED(arg1_type))
	arg1 = bb->build_inst(Op::ZEXT, arg1, bitsize_inst);
      else
	arg1 = bb->build_inst(Op::SEXT, arg1, bitsize_inst);
      if (TYPE_UNSIGNED(arg2_type))
	arg2 = bb->build_inst(Op::ZEXT, arg2, bitsize_inst);
      else
	arg2 = bb->build_inst(Op::SEXT, arg2, bitsize_inst);
      Instruction *inst;
      if (name == "ADD_OVERFLOW"s)
	inst = bb->build_inst(Op::ADD, arg1, arg2);
      else if (name == "SUB_OVERFLOW"s)
	inst = bb->build_inst(Op::SUB, arg1, arg2);
      else
	inst = bb->build_inst(Op::MUL, arg1, arg2);
      Instruction *res = bb->build_trunc(inst, lhs_elem_bitsize);
      Instruction *eres;
      if (TYPE_UNSIGNED(lhs_elem_type))
	eres = bb->build_inst(Op::ZEXT, res, bitsize_inst);
      else
	eres = bb->build_inst(Op::SEXT, res, bitsize_inst);
      Instruction *overflow = bb->build_inst(Op::NE, inst, eres);

      res = to_mem_repr(bb, res, lhs_elem_type);
      Instruction *res_bitsize_inst = bb->value_inst(res->bitsize, 32);
      overflow = bb->build_inst(Op::ZEXT, overflow, res_bitsize_inst);
      res = bb->build_inst(Op::CONCAT, overflow, res);
      constrain_range(bb, lhs, res);
      tree2instruction[lhs] = res;
      return;
    }

  if (name == "BUILTIN_EXPECT"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;

      Instruction *arg = tree2inst_undefcheck(bb, gimple_call_arg(stmt, 0));
      constrain_range(bb, lhs, arg);
      tree2instruction[lhs] = arg;
      return;
    }

  if (name == "CLZ"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      int bitsize = bitsize_for_type(TREE_TYPE(lhs));
      tree arg_expr = gimple_call_arg(stmt, 0);
      Instruction *arg = tree2inst_undefcheck(bb, arg_expr);

      Instruction *val0;
      int value = 0;
      if (CLZ_DEFINED_VALUE_AT_ZERO(SCALAR_INT_TYPE_MODE(TREE_TYPE(arg_expr)), value))
	val0 = bb->value_inst(value, bitsize);
      else
	{
	  if (!state->clz_bitsize2idx.contains(bitsize))
	    state->clz_bitsize2idx[bitsize] = state->symbolic_idx++;
	  uint32_t idx = state->clz_bitsize2idx[bitsize];
	  Instruction *idx_inst = bb->value_inst(idx, 32);
	  Instruction *bitsize_inst = bb->value_inst(bitsize, 32);
	  val0 = bb->build_inst(Op::SYMBOLIC, idx_inst, bitsize_inst);
	}

      Instruction *inst = val0;
      for (unsigned i = 0; i < arg->bitsize; i++)
	{
	  Instruction *bit = bb->build_extract_bit(arg, i);
	  Instruction *val = bb->value_inst(arg->bitsize - i - 1, bitsize);
	  inst = bb->build_inst(Op::ITE, bit, val, inst);
	}
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "CTZ"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      int bitsize = bitsize_for_type(TREE_TYPE(lhs));
      tree arg_expr = gimple_call_arg(stmt, 0);
      Instruction *arg = tree2inst_undefcheck(bb, arg_expr);

      Instruction *val0;
      int value = 0;
      if (CLZ_DEFINED_VALUE_AT_ZERO(SCALAR_INT_TYPE_MODE(TREE_TYPE(arg_expr)), value))
	val0 = bb->value_inst(value, bitsize);
      else
	{
	  if (!state->clz_bitsize2idx.contains(bitsize))
	    state->clz_bitsize2idx[bitsize] = state->symbolic_idx++;
	  uint32_t idx = state->clz_bitsize2idx[bitsize];
	  Instruction *idx_inst = bb->value_inst(idx, 32);
	  Instruction *bitsize_inst = bb->value_inst(bitsize, 32);
	  val0 = bb->build_inst(Op::SYMBOLIC, idx_inst, bitsize_inst);
	}

      Instruction *inst = val0;
      for (int i = arg->bitsize - 1; i >= 0; i--)
	{
	  Instruction *bit = bb->build_extract_bit(arg, i);
	  Instruction *val = bb->value_inst(i, bitsize);
	  inst = bb->build_inst(Op::ITE, bit, val, inst);
	}
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "DIVMOD"s)
    {
      tree arg1_expr = gimple_call_arg(stmt, 0);
      tree arg1_type = TREE_TYPE(arg1_expr);
      tree arg2_expr = gimple_call_arg(stmt, 1);
      tree arg2_type = TREE_TYPE(arg2_expr);
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      tree lhs_elem_type = TREE_TYPE(TREE_TYPE(lhs));
      Instruction *arg1 = tree2inst_undefcheck(bb, arg1_expr);
      Instruction *arg2 = tree2inst_undefcheck(bb, arg2_expr);
      Instruction *mod = process_binary_scalar(TRUNC_MOD_EXPR, arg1, arg2,
					       lhs_elem_type, arg1_type,
					       arg2_type, bb);
      mod = to_mem_repr(bb, mod, lhs_elem_type);
      Instruction *div = process_binary_scalar(TRUNC_DIV_EXPR, arg1, arg2,
					       lhs_elem_type, arg1_type,
					       arg2_type, bb);
      div = to_mem_repr(bb, div, lhs_elem_type);
      Instruction *inst = bb->build_inst(Op::CONCAT, mod, div);
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "LOOP_VECTORIZED"s)
    {
      tree lhs = gimple_call_lhs(stmt);
      assert(lhs);
      Instruction *idx_inst = bb->value_inst(state->symbolic_idx++, 32);
      Instruction *bitsize_inst = bb->value_inst(1, 32);
      Instruction *inst = bb->build_inst(Op::SYMBOLIC, idx_inst, bitsize_inst);
      tree2instruction[lhs] = inst;
      return;
    }

  if (name == "VCOND_MASK"s)
    {
      tree arg1_expr = gimple_call_arg(stmt, 0);
      tree arg1_type = TREE_TYPE(arg1_expr);
      tree arg2_expr = gimple_call_arg(stmt, 1);
      tree arg2_type = TREE_TYPE(arg2_expr);
      tree arg3_expr = gimple_call_arg(stmt, 2);
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;

      Instruction *arg1 = tree2inst_undefcheck(bb, arg1_expr);
      auto [arg2, arg2_undef] = tree2inst(bb, arg2_expr);
      auto [arg3, arg3_undef] = tree2inst(bb, arg3_expr);
      auto [inst, undef] = process_vec_cond(arg1, arg2, arg2_undef,
					    arg3, arg3_undef, arg1_type,
					    arg2_type, bb);
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      if (undef)
	tree2undef[lhs] = undef;
      return;
    }

  if (name == "VCOND"s || name == "VCONDU"s)
    {
      tree arg1_expr = gimple_call_arg(stmt, 0);
      tree arg1_type = TREE_TYPE(arg1_expr);
      tree arg1_elem_type = TREE_TYPE(arg1_type);
      tree arg2_expr = gimple_call_arg(stmt, 1);
      tree arg2_type = TREE_TYPE(arg2_expr);
      tree arg2_elem_type = TREE_TYPE(arg2_type);
      tree arg3_expr = gimple_call_arg(stmt, 2);
      tree arg3_type = TREE_TYPE(arg3_expr);
      tree arg3_elem_type = TREE_TYPE(arg3_type);
      tree arg4_expr = gimple_call_arg(stmt, 3);
      tree arg5_expr = gimple_call_arg(stmt, 4);
      tree lhs = gimple_call_lhs(stmt);

      Instruction *arg1 = tree2inst_undefcheck(bb, arg1_expr);
      Instruction *arg2 = tree2inst_undefcheck(bb, arg2_expr);
      auto [arg3, arg3_undef] = tree2inst(bb, arg3_expr);
      auto [arg4, arg4_undef] = tree2inst(bb, arg4_expr);
      if (arg3_undef || arg4_undef)
	{
	  if (!arg3_undef)
	    arg3_undef = bb->value_inst(0, arg3->bitsize);
	  if (!arg4_undef)
	    arg4_undef = bb->value_inst(0, arg4->bitsize);
	}
      assert(arg1->bitsize == arg2->bitsize);
      assert(arg3->bitsize == arg4->bitsize);

      enum tree_code code = (enum tree_code)get_int_cst_val(arg5_expr);
      bool is_unsigned = name == "VCONDU";

      uint32_t elem_bitsize1 = bitsize_for_type(arg1_elem_type);
      uint32_t elem_bitsize3 = bitsize_for_type(arg3_elem_type);

      Instruction *res = nullptr;
      uint32_t nof_elt = bitsize_for_type(arg1_type) / elem_bitsize1;
      for (uint64_t i = 0; i < nof_elt; i++)
	{
	  Instruction *a1 = extract_vec_elem(bb, arg1, elem_bitsize1, i);
	  Instruction *a2 = extract_vec_elem(bb, arg2, elem_bitsize1, i);
	  Instruction *a3 = extract_vec_elem(bb, arg3, elem_bitsize3, i);
	  Instruction *a4 = extract_vec_elem(bb, arg4, elem_bitsize3, i);

	  Instruction *cond;
	  if (FLOAT_TYPE_P(arg1_elem_type))
	    cond = process_binary_float(code, a1, a2, bb);
	  else
	    cond = process_binary_int(code, is_unsigned, a1, a2,
				      boolean_type_node, arg1_elem_type,
				      arg2_elem_type, bb);
	  Instruction *inst = bb->build_inst(Op::ITE, cond, a3, a4);
	  if (res)
	    res = bb->build_inst(Op::CONCAT, inst, res);
	  else
	    res = inst;

	  if (arg3_undef)
	    {
	      Instruction *a3_undef =
		extract_vec_elem(bb, arg3_undef, elem_bitsize3, i);
	      Instruction *a4_undef =
		extract_vec_elem(bb, arg4_undef, elem_bitsize3, i);
	      Instruction *undef =
		bb->build_inst(Op::ITE, cond, a3_undef, a4_undef);
	      Instruction *zero = bb->value_inst(0, undef->bitsize);
	      Instruction *cmp = bb->build_inst(Op::NE, undef, zero);
	      bb->build_inst(Op::UB, cmp);
	    }
	}
      if (lhs)
	{
	  constrain_range(bb, lhs, res);
	  tree2instruction[lhs] = res;
	}
      return;
    }

  if (name == "VEC_CONVERT"s)
    {
      tree arg1_expr = gimple_call_arg(stmt, 0);
      Instruction *arg1 = tree2inst_undefcheck(bb, arg1_expr);
      tree arg1_elem_type = TREE_TYPE(TREE_TYPE(arg1_expr));
      tree lhs = gimple_call_lhs(stmt);
      if (!lhs)
	return;
      tree lhs_elem_type = TREE_TYPE(TREE_TYPE(lhs));
      auto [inst, undef] =
	process_unary_vec(CONVERT_EXPR, arg1, nullptr, lhs_elem_type,
			  arg1_elem_type, bb);
      assert(!undef);
      constrain_range(bb, lhs, inst);
      tree2instruction[lhs] = inst;
      return;
    }

  throw Not_implemented("process_gimple_call_internal: "s + name);
}

void Converter::process_gimple_call(gimple *stmt, Basic_block *bb)
{
  if (gimple_call_builtin_p(stmt))
    process_gimple_call_builtin(stmt, bb);
  else if (gimple_call_internal_p(stmt))
    process_gimple_call_internal(stmt, bb);
  else
    throw Not_implemented("gimple_call");
}

Instruction *Converter::build_label_cond(tree index_expr, tree label, Basic_block *bb)
{
  tree index_type = TREE_TYPE(index_expr);
  Instruction *index = tree2inst_undefcheck(bb, index_expr);
  tree low_expr = CASE_LOW(label);
  Instruction *low = tree2inst_undefcheck(bb, low_expr);
  low = type_convert(low, TREE_TYPE(low_expr), index_type, bb);
  tree high_expr = CASE_HIGH(label);
  Instruction *cond;
  if (high_expr)
    {
      Instruction *high = tree2inst_undefcheck(bb, high_expr);
      high = type_convert(high, TREE_TYPE(high_expr), index_type, bb);
      Op op = TYPE_UNSIGNED(index_type) ?  Op::UGE: Op::SGE;
      Instruction *cond_low = bb->build_inst(op, index, low);
      Instruction *cond_high = bb->build_inst(op, high, index);
      cond = bb->build_inst(Op::AND, cond_low, cond_high);
    }
  else
    cond = bb->build_inst(Op::EQ, index, low);
  return cond;
}

// Expand switch statements to a series of compare and branch.
void Converter::process_gimple_switch(gimple *stmt, Basic_block *switch_bb)
{
  gswitch *switch_stmt = as_a<gswitch *>(stmt);
  tree index_expr = gimple_switch_index(switch_stmt);

  // We expand the switch case to a series of compare and branch. This
  // complicates the phi node handling -- phi arguments from the BB
  // containing the switch statement should use the correct BB in the compare
  // and branch chain, so we must keep track of which new BBs corresponds
  // to the switch statement.
  std::set<Basic_block *>& bbset = switch_bbs[switch_bb];

  // We start the chain by an unconditional branch to a new BB instead of
  // doing the first compare-and-branch at the end of the BB containing the
  // switch statement. This is not necessary, but it avoids confusion as
  // the phi argument from switch always comes from a BB we have introduced.
  Basic_block *bb = func->build_bb();
  bbset.insert(bb);
  switch_bb->build_br_inst(bb);

  // Multiple switch cases may branch to the same basic block. Collect these
  // so that we only do one branch (in order to prevent complications when
  // the target contains phi nodes that would otherwise need to be adjusted
  // for the additional edges).
  basic_block default_block = gimple_switch_label_bb(fun, switch_stmt, 0);
  std::map<basic_block, std::vector<tree>> block2labels;
  size_t n = gimple_switch_num_labels(switch_stmt);
  std::vector<basic_block> cases;
  for (size_t i = 1; i < n; i++)
    {
      tree label = gimple_switch_label(switch_stmt, i);
      basic_block block = label_to_block(fun, CASE_LABEL(label));
      if (block == default_block)
	continue;
      if (!block2labels.contains(block))
	cases.push_back(block);
      block2labels[block].push_back(label);
    }

  if (cases.empty())
    {
      // All cases branch to the default case.
      bb->build_br_inst(gccbb2bb.at(default_block));
      return;
    }

  n = cases.size();
  for (size_t i = 0; i < n; i++)
    {
      Instruction *cond = nullptr;
      basic_block block = cases[i];
      const std::vector<tree>& labels = block2labels.at(block);
      for (auto label : labels)
	{
	  Instruction *label_cond = build_label_cond(index_expr, label, bb);
	  if (cond)
	    cond = bb->build_inst(Op::OR, cond, label_cond);
	  else
	    cond = label_cond;
	}

      Basic_block *true_bb = gccbb2bb.at(block);
      Basic_block *false_bb;
      if (i != n - 1)
	{
	  false_bb = func->build_bb();
	  bbset.insert(false_bb);
	}
      else
	false_bb = gccbb2bb.at(default_block);
      bb->build_br_inst(cond, true_bb, false_bb);
      bb = false_bb;
    }
}

// Get the BB corresponding to the source of the phi argument i.
Basic_block *Converter::get_phi_arg_bb(gphi *phi, int i)
{
  edge e = gimple_phi_arg_edge(phi, i);
  Basic_block *arg_bb = gccbb2bb.at(e->src);
  Basic_block *phi_bb = gccbb2bb.at(e->dest);
  if (switch_bbs.contains(arg_bb))
    {
      std::set<Basic_block *>& bbset = switch_bbs[arg_bb];
      assert(bbset.size() > 0);
      for (auto bb : bbset)
	{
	  auto it = std::find(phi_bb->preds.begin(), phi_bb->preds.end(), bb);
	  if (it != phi_bb->preds.end())
	    return bb;
	}
      assert(false);
    }
  return arg_bb;
}

void Converter::process_gimple_return(gimple *stmt, Basic_block *bb)
{
  greturn *return_stmt = dyn_cast<greturn *>(stmt);
  tree expr = gimple_return_retval(return_stmt);
  if (expr)
    bb2retval[bb] = tree2inst(bb, expr);
  // TODO: Add assert that the successor goes to the exit block. We will
  // miscompile otherwise...
}

void Converter::generate_return_inst(Basic_block *bb)
{
  if (!retval_bitsize)
    {
      bb->build_ret_inst();
      return;
    }

  // Some predecessors to the exit block may not have a return value;
  // They may have a return without value, or the predecessor may be
  // a builtin_unreachable, etc. We therefore creates a dummy value,
  // marked as undefined, for these predecessors to make the IR valid.
  {
    Instruction *retval = nullptr;
    Instruction *undef = nullptr;
    Basic_block *entry_bb = func->bbs[0];
    for (Basic_block *pred_bb : bb->preds)
      {
	if (!bb2retval.contains(pred_bb))
	  {
	    if (!retval)
	      {
		retval = entry_bb->value_inst(0, retval_bitsize);
		unsigned bitsize = retval_bitsize;
		while (bitsize)
		  {
		    uint32_t bs = std::min(bitsize, 128u);
		    bitsize -= bs;
		    Instruction *inst = entry_bb->value_inst(-1, bs);
		    if (undef)
		      undef = entry_bb->build_inst(Op::CONCAT, inst, undef);
		    else
		      undef = inst;
		  }
	      }
	    bb2retval[pred_bb] = {retval, undef};
	  }
      }
  }

  Instruction *retval;
  Instruction *retval_undef;
  if (bb->preds.size() == 1)
    std::tie(retval, retval_undef) = bb2retval.at(bb->preds[0]);
  else
    {
      Instruction *phi = bb->build_phi_inst(retval_bitsize);
      Instruction *phi_undef = bb->build_phi_inst(retval_bitsize);
      bool need_undef_phi = false;
      for (Basic_block *pred_bb : bb->preds)
	{
	  auto [ret, ret_undef] = bb2retval.at(pred_bb);
	  phi->add_phi_arg(ret, pred_bb);
	  need_undef_phi = need_undef_phi || ret_undef;
	  if (!ret_undef)
	    ret_undef = pred_bb->value_inst(0, retval_bitsize);
	  phi_undef->add_phi_arg(ret_undef, pred_bb);
	}
      retval = phi;
      retval_undef = need_undef_phi ? phi_undef : nullptr;
    }

  // GCC treats it as UB to return the address of a local variable.
  if (POINTER_TYPE_P(retval_type))
    {
      uint32_t ptr_id_bits = func->module->ptr_id_bits;
      Instruction *mem_id = bb->build_extract_id(retval);
      Instruction *zero = bb->value_inst(0, ptr_id_bits);
      Instruction *cond = bb->build_inst(Op::SLT, mem_id, zero);
      if (retval_undef)
	{
	  Instruction *zero2 = bb->value_inst(0, retval_undef->bitsize);
	  Instruction *cond2 = bb->build_inst(Op::EQ, retval_undef, zero2);
	  cond = bb->build_inst(Op::AND, cond, cond2);
	}
      bb->build_inst(Op::UB, cond);
    }

  if (retval_undef)
    bb->build_ret_inst(retval, retval_undef);
  else
    bb->build_ret_inst(retval);
}

void Converter::xxx_constructor(tree initial, Instruction *mem_inst)
{
  Basic_block *bb = mem_inst->bb;
  Instruction *ptr = mem_inst;
  tree type = TREE_TYPE(initial);
  uint64_t size = bytesize_for_type(TREE_TYPE(initial));

  if (TREE_CODE(initial) == STRING_CST)
    {
      uint64_t len = TREE_STRING_LENGTH(initial);
      const char *p = TREE_STRING_POINTER(initial);
      ptr = mem_inst;
      Instruction *one = bb->value_inst(1, ptr->bitsize);
      for (uint64_t i = 0; i < len; i++)
	{
	  Instruction *byte = bb->value_inst(p[i], 8);
	  bb->build_inst(Op::STORE, ptr, byte);
	  ptr = bb->build_inst(Op::ADD, ptr, one);
	}
      return;
    }

  if (INTEGRAL_TYPE_P(type)
      || TREE_CODE(type) == OFFSET_TYPE
      || FLOAT_TYPE_P(type)
      || POINTER_TYPE_P(type)
      || VECTOR_TYPE_P(type))
    {
      Instruction *value = tree2inst_constructor(bb, initial);
      value = to_mem_repr(bb, value, type);
      store_value(bb, mem_inst, value);
      return;
    }

  if (TREE_CODE(type) == ARRAY_TYPE)
    {
      tree elem_type = TREE_TYPE(type);
      uint64_t elem_size = bytesize_for_type(elem_type);
      unsigned HOST_WIDE_INT idx;
      tree index;
      tree value;
      FOR_EACH_CONSTRUCTOR_ELT(CONSTRUCTOR_ELTS(initial), idx, index, value)
	{
	  if (index && TREE_CODE(index) == RANGE_EXPR)
	    throw Not_implemented("init_var: RANGE_EXPR");
	  uint64_t offset = idx * elem_size;
	  Instruction *off = bb->value_inst(offset, ptr->bitsize);
	  Instruction *ptr2 = bb->build_inst(Op::ADD, ptr, off);
	  xxx_constructor(value, ptr2);
	}
      return;
    }

  if (TREE_CODE(type) == RECORD_TYPE || TREE_CODE(type) == UNION_TYPE)
    {
      unsigned HOST_WIDE_INT idx;
      tree index;
      tree value;
      FOR_EACH_CONSTRUCTOR_ELT(CONSTRUCTOR_ELTS(initial), idx, index, value)
	{
	  uint64_t offset = get_int_cst_val(DECL_FIELD_OFFSET(index));
	  uint64_t bit_offset = get_int_cst_val(DECL_FIELD_BIT_OFFSET(index));
	  offset += bit_offset / 8;
	  bit_offset &= 7;
	  Instruction *off = bb->value_inst(offset, ptr->bitsize);
	  Instruction *ptr2 = bb->build_inst(Op::ADD, ptr, off);
	  tree elem_type = TREE_TYPE(value);
	  if (TREE_CODE(elem_type) == ARRAY_TYPE
	      || TREE_CODE(elem_type) == RECORD_TYPE
	      || TREE_CODE(elem_type) == UNION_TYPE)
	    xxx_constructor(value, ptr2);
	  else
	    {
	      uint64_t bitsize = bitsize_for_type(elem_type);
	      Instruction *value_inst = tree2inst_constructor(bb, value);
	      size = (bitsize + bit_offset + 7) / 8;
	      if (DECL_BIT_FIELD_TYPE(index))
		{
		  if (bit_offset)
		    {
		      Instruction *first_byte = bb->build_inst(Op::LOAD, ptr2);
		      Instruction *bits = bb->build_trunc(first_byte, bit_offset);
		      value_inst = bb->build_inst(Op::CONCAT, value_inst, bits);
		    }
		  if (bitsize + bit_offset != size * 8)
		    {
		      Instruction *offset =
			bb->value_inst(size - 1, ptr2->bitsize);
		      Instruction *ptr3 = bb->build_inst(Op::ADD, ptr2, offset);

		      uint64_t remaining = size * 8 - (bitsize + bit_offset);
		      assert(remaining < 8);
		      Instruction *high = bb->value_inst(7, 32);
		      Instruction *low = bb->value_inst(8 - remaining, 32);

		      Instruction *last_byte =
			bb->build_inst(Op::LOAD, ptr3);
		      Instruction *bits =
			bb->build_inst(Op::EXTRACT, last_byte, high, low);
		      value_inst = bb->build_inst(Op::CONCAT, bits, value_inst);
		    }
		}
	      else
		{
		  value_inst = to_mem_repr(bb, value_inst, elem_type);
		}
	      store_value(bb, ptr2, value_inst);
	    }
	}
      return;
    }

  throw Not_implemented("init_var: unknown constructor");
}

void Converter::init_var(tree decl, Instruction *mem_inst)
{
  uint64_t size = bytesize_for_type(TREE_TYPE(decl));
  if (size > MAX_MEMORY_UNROLL_LIMIT)
    throw Not_implemented("init_var: too large constructor");
  check_type(TREE_TYPE(decl));

  Basic_block *bb = mem_inst->bb;

  tree initial = DECL_INITIAL(decl);
  if (!initial)
    {
      if (!TREE_STATIC(decl))
	return;

      // Uninitializied static variables are guaranted to be initialized to 0.
      Instruction *ptr = mem_inst;
      Instruction *zero = bb->value_inst(0, 8);
      Instruction *one = bb->value_inst(1, ptr->bitsize);
      uint64_t size = bytesize_for_type(TREE_TYPE(decl));
      for (uint64_t i = 0; i < size; i++)
	{
	  bb->build_inst(Op::STORE, ptr, zero);
	  ptr = bb->build_inst(Op::ADD, ptr, one);
	}
      return;
    }

  if (TREE_CODE(initial) == CONSTRUCTOR)
    {
      assert(TREE_CODE(initial) == CONSTRUCTOR);
      tree type = TREE_TYPE(initial);
      uint64_t size = bytesize_for_type(TREE_TYPE(initial));

      if (CONSTRUCTOR_NO_CLEARING(initial))
	throw Not_implemented("init_var: CONSTRUCTOR_NO_CLEARING");

      Instruction *ptr = mem_inst;
      Instruction *zero = bb->value_inst(0, 8);
      Instruction *one = bb->value_inst(1, ptr->bitsize);
      if (size > MAX_MEMORY_UNROLL_LIMIT)
	throw Not_implemented("init_var: too large constructor");
      for (uint64_t i = 0; i < size; i++)
	{
	  uint8_t padding = padding_at_offset(type, i);
	  if (padding)
	    bb->build_inst(Op::SET_MEM_UNDEF, ptr, bb->value_inst(padding, 8));
	  if (padding != 255)
	    bb->build_inst(Op::STORE, ptr, zero);
	  ptr = bb->build_inst(Op::ADD, ptr, one);
	}
    }

  xxx_constructor(initial, mem_inst);
}

void Converter::make_uninit(Basic_block *bb, Instruction *ptr, uint64_t size)
{
  Instruction *one = bb->value_inst(1, ptr->bitsize);
  Instruction *byte_m1 = bb->value_inst(255, 8);
  for (uint64_t i = 0; i < size; i++)
    {
      bb->build_inst(Op::SET_MEM_UNDEF, ptr, byte_m1);
      ptr = bb->build_inst(Op::ADD, ptr, one);
    }
}

void Converter::process_variables()
{
  tree retval_decl = DECL_RESULT(fun->decl);
  retval_type = TREE_TYPE(retval_decl);
  if (VOID_TYPE_P(retval_type))
    retval_bitsize = 0;
  else
    {
      retval_bitsize = bitsize_for_type(TREE_TYPE(DECL_RESULT(fun->decl)));

      uint64_t id;
      if (state->decl2id.contains(retval_decl))
	{
	  id = state->decl2id.at(retval_decl);
	}
      else
	{
	  id = --state->id_local;
	  state->decl2id[retval_decl] = id;
	}
      uint64_t size = bytesize_for_type(retval_type);
      Instruction *memory_inst = build_memory_inst(id, size, MEM_UNINIT);
      decl2instruction[retval_decl] = memory_inst;
    }

  // Add an anonymous memory as first global.
  // TODO: Should only be done if we have unconstrained pointers?
  build_memory_inst(2, ANON_MEM_SIZE, MEM_KEEP);

  // Global variables.
  {
    varpool_node *var;
    std::map<std::string, tree> name2decl;
    FOR_EACH_VARIABLE(var)
      {
	tree decl = var->decl;
	if (lookup_attribute("alias", DECL_ATTRIBUTES(decl)))
	  continue;
	uint64_t size = bytesize_for_type(TREE_TYPE(decl));
	if (size > MAX_MEMORY_UNROLL_LIMIT)
	  throw Not_implemented("process_function: too large global variable");
	// TODO: Implement.
	if (size == 0)
	  throw Not_implemented("process_function: unknown size");

	uint64_t id;
	if (state->decl2id.contains(decl))
	  {
	    id = state->decl2id.at(decl);
	  }
	else
	  {
	    uint32_t ptr_id_bits = func->module->ptr_id_bits;

	    // Artificial decls are used for data introduced by the compiler
	    // (such as switch tables), so normal, unconstrained, pointers
	    // cannot point to them. Give these a local ID.
	    if (DECL_ARTIFICIAL(decl))
	      {
		if (state->id_local <= -(1 << ((ptr_id_bits - 1))))
		  throw Not_implemented("too many local variables");
		id = --state->id_local;
	      }
	    else
	      {
		if (state->id_global >= (1 << ((ptr_id_bits - 1) - 1)))
		  throw Not_implemented("too many global variables");
		id = ++state->id_global;
	      }
	    state->decl2id[decl] = id;
	  }
	uint64_t flags = 0;
	if (TREE_READONLY(decl))
	  flags |= MEM_CONST;
	Instruction *memory_inst = build_memory_inst(id, size, flags);
	decl2instruction[decl] = memory_inst;
	if (DECL_NAME(decl))
	  {
	    const char *name = IDENTIFIER_POINTER(DECL_NAME(decl));
	    name2decl[name] = decl;
	  }
      }

    FOR_EACH_VARIABLE(var)
      {
	tree decl = var->decl;
	tree alias = lookup_attribute("alias", DECL_ATTRIBUTES(decl));
	if (alias)
	  {
	    const char *name = IDENTIFIER_POINTER(DECL_NAME(decl));
	    const char *alias_name =
	      TREE_STRING_POINTER(TREE_VALUE(TREE_VALUE(alias)));
	    if (!name2decl.contains(alias_name))
	      throw Not_implemented("unknown alias");
	    tree alias_decl = name2decl.at(alias_name);
	    decl2instruction[decl] = decl2instruction.at(alias_decl);
	    name2decl[name] = alias_decl;
	  }
      }

    // Must do this after creating all variables as a pointer may need to
    // be initialized by an address of a later variable.
    FOR_EACH_VARIABLE(var)
      {
	tree decl = var->decl;
	if (TREE_READONLY(decl))
	  init_var(decl, decl2instruction.at(decl));
      }
  }

  // Local variables.
  {
    tree decl;
    unsigned ix;
    FOR_EACH_LOCAL_DECL(fun, ix, decl)
      {
	// Local static decls are included in the global decls, so their
	// memory objects have already been created.
	if (decl2instruction.contains(decl))
	  {
	    assert(TREE_STATIC(decl));
	    continue;
	  }

	assert(!DECL_INITIAL(decl));

	uint64_t size = bytesize_for_type(TREE_TYPE(decl));
	if (size > MAX_MEMORY_UNROLL_LIMIT)
	  throw Not_implemented("process_function: too large local variable");

	int64_t id;
	if (state->decl2id.contains(decl))
	  {
	    id = state->decl2id.at(decl);
	  }
	else
	  {
	    uint32_t ptr_id_bits = func->module->ptr_id_bits;
	    if (state->id_local <= -(1 << ((ptr_id_bits - 1))))
	      throw Not_implemented("too many local variables");
	    id = --state->id_local;
	    state->decl2id[decl] = id;
	  }
	uint64_t flags = MEM_UNINIT;
	if (TREE_READONLY(decl))
	  flags |= MEM_CONST;
	Instruction *memory_inst = build_memory_inst(id, size, flags);
	decl2instruction[decl] = memory_inst;
      }
  }
}

void Converter::process_func_args()
{
  tree fntype = TREE_TYPE(fun->decl);
  bitmap nonnullargs = get_nonnull_args(fntype);
  tree decl;
  int param_number = 0;
  Basic_block *bb = func->bbs[0];
  const char *decl_name = IDENTIFIER_POINTER(DECL_NAME(fun->decl));
  for (decl = DECL_ARGUMENTS(fun->decl); decl; decl = DECL_CHAIN(decl))
    {
      check_type(TREE_TYPE(decl));
      int bitsize = bitsize_for_type(TREE_TYPE(decl));
      if (bitsize <= 0)
	throw Not_implemented("Parameter size == 0");

      bool type_is_unsigned =
	TREE_CODE(TREE_TYPE(decl)) == INTEGER_TYPE
	&& TYPE_UNSIGNED(TREE_TYPE(decl))
	&& TYPE_PRECISION(TREE_TYPE(decl)) != 32;
      state->param_is_unsigned.push_back(type_is_unsigned);

      // TODO: There must be better ways to determine if this is the "this"
      // pointer of a C++ constructor.
      if (param_number == 0 && !strcmp(decl_name, "__ct_base "))
	{
	  assert(POINTER_TYPE_P(TREE_TYPE(decl)));

	  // We use constant ID as it must be the same between src and tgt.
	  int64_t id = 1;
	  uint64_t flags = MEM_UNINIT | MEM_KEEP;
	  uint64_t size = bytesize_for_type(TREE_TYPE(TREE_TYPE(decl)));

	  Instruction *param_inst = build_memory_inst(id, size, flags);
	  tree2instruction[decl] = param_inst;
	}
      else
	{
	  Instruction *param_nbr = bb->value_inst(param_number, 32);
	  Instruction *param_bitsize = bb->value_inst(bitsize, 32);
	  Instruction *param_inst =
	    bb->build_inst(Op::PARAM, param_nbr, param_bitsize);
	  tree2instruction[decl] = param_inst;

	  // Pointers cannot point to local variables or to the this pointer
	  // in constructors.
	  // TODO: Update all pointer UB checks for this.
	  if (POINTER_TYPE_P(TREE_TYPE(decl)))
	    {
	      uint32_t ptr_id_bits = func->module->ptr_id_bits;
	      Instruction *id = bb->build_extract_id(param_inst);
	      Instruction *zero = bb->value_inst(0, ptr_id_bits);
	      Instruction *cond0 = bb->build_inst(Op::SLT, id, zero);
	      Instruction *one = bb->value_inst(1, ptr_id_bits);
	      Instruction *cond1 = bb->build_inst(Op::EQ, id, one);
	      Instruction *cond = bb->build_inst(Op::OR, cond0, cond1);
	      bb->build_inst(Op::UB, cond);
	    }

	  canonical_nan_check(bb, param_inst, TREE_TYPE(decl), nullptr);

	  // Params marked "nonnull" is UB if NULL.
	  if (POINTER_TYPE_P(TREE_TYPE(decl))
	      && nonnullargs
	      && (bitmap_empty_p(nonnullargs)
		  || bitmap_bit_p(nonnullargs, param_number)))
	    {
	      Instruction *zero = bb->value_inst(0, param_inst->bitsize);
	      Instruction *cond = bb->build_inst(Op::EQ, param_inst, zero);
	      bb->build_inst(Op::UB, cond);
	    }

	  // VRP
	  // If there are recorded data, we get a constant value, and a mask
	  // indicating which bits varies. For example, for a funcion
	  //   static void foo(uint16_t x);
	  // is called as
	  //   foo(0xfffe);
	  //   foo(0xf0ff);
	  // we get value == 0xf0fe and mask == 0xf01
	  tree value;
	  widest_int mask;
	  if (ipcp_get_parm_bits(decl, &value, &mask))
	    {
	      unsigned __int128 m = get_widest_int_val(mask);
	      unsigned __int128 v = get_int_cst_val(value);
	      assert((m & v) == 0);

	      Instruction *m_inst = bb->value_inst(~m, param_inst->bitsize);
	      Instruction *v_inst = bb->value_inst(v, param_inst->bitsize);
	      Instruction *and_inst =
		bb->build_inst(Op::AND, param_inst, m_inst);
	      Instruction *cond = bb->build_inst(Op::NE, v_inst, and_inst);
	      bb->build_inst(Op::UB, cond);
	    }
	}
      param_number++;
    }
  BITMAP_FREE(nonnullargs);
}

void Converter::process_instructions(int nof_blocks, int *postorder)
{
  for (int i = 0; i < nof_blocks; i++) {
    basic_block gcc_bb =
      (*fun->cfg->x_basic_block_info)[postorder[nof_blocks - 1 - i]];
    Basic_block *bb = gccbb2bb.at(gcc_bb);
    gimple *switch_stmt = nullptr;
    gimple *cond_stmt = nullptr;
    gimple_stmt_iterator gsi;
    for (gsi = gsi_start_phis(gcc_bb); !gsi_end_p(gsi); gsi_next(&gsi))
      {
	gimple *phi = gsi_stmt(gsi);
	tree phi_result = gimple_phi_result(phi);
	if (VOID_TYPE_P(TREE_TYPE(phi_result)))
	  {
	    // Skip phi nodes for the memory SSA virtual SSA names.
	    continue;
	  }
	int bitwidth = bitsize_for_type(TREE_TYPE(phi_result));
	Instruction *phi_inst = bb->build_phi_inst(bitwidth);
	Instruction *phi_undef = bb->build_phi_inst(bitwidth);
	constrain_range(bb, phi_result, phi_inst, phi_undef);
	tree2instruction[phi_result] = phi_inst;
	tree2undef[phi_result] = phi_undef;
      }
    for (gsi = gsi_start_bb(gcc_bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
      gimple *stmt = gsi_stmt(gsi);
      switch (gimple_code(stmt))
	{
	case GIMPLE_ASSIGN:
	  process_gimple_assign(stmt, bb);
	  break;
	case GIMPLE_ASM:
	  process_gimple_asm(stmt);
	  break;
	case GIMPLE_CALL:
	  process_gimple_call(stmt, bb);
	  break;
	case GIMPLE_COND:
	  assert(!cond_stmt);
	  assert(!switch_stmt);
	  cond_stmt = stmt;
	  break;
	case GIMPLE_RETURN:
	  process_gimple_return(stmt, bb);
	  break;
	case GIMPLE_SWITCH:
	  assert(!cond_stmt);
	  assert(!switch_stmt);
	  switch_stmt = stmt;
	  break;
	case GIMPLE_LABEL:
	case GIMPLE_PREDICT:
	case GIMPLE_NOP:
	  // Nothing to do.
	  break;
	default:
	  {
	    const char *name = gimple_code_name[gimple_code(stmt)];
	    throw Not_implemented("process_instructions: "s + name);
	  }
	}
    }

    // Check that we do not have any extra, unsupported, edges as that will
    // make the code below fail assertions when adding the branches.
    for (unsigned j = 0; j < EDGE_COUNT(gcc_bb->succs); j++)
      {
	edge succ_edge = EDGE_SUCC(gcc_bb, j);
	if (succ_edge->flags & EDGE_ABNORMAL)
	  throw Not_implemented("abnormal edge(exceptions)");
	if (succ_edge->flags & EDGE_IRREDUCIBLE_LOOP)
	  throw Not_implemented("irreducible loop");
      }

    // Add the branch instruction(s) at the end of the basic block.
    if (switch_stmt)
      process_gimple_switch(switch_stmt, bb);
    else if (EDGE_COUNT(gcc_bb->succs) == 0)
      {
	basic_block gcc_exit_block = EXIT_BLOCK_PTR_FOR_FN(fun);
	if (gcc_bb != gcc_exit_block)
	  {
	    // This is not the exit block, but there are not any successors
	    // (I.e., this is a block from an __builting_unreachable() etc.)
	    // so we must add a branch to the real exit block as the smtgcc
	    // IR only can have one ret instruction.
	    bb->build_br_inst(gccbb2bb.at(gcc_exit_block));
	  }
	else
	  generate_return_inst(bb);
      }
    else if (cond_stmt)
      {
	tree_code code = gimple_cond_code(cond_stmt);
	tree arg1_expr = gimple_cond_lhs(cond_stmt);
	tree arg2_expr = gimple_cond_rhs(cond_stmt);
	tree arg1_type = TREE_TYPE(arg1_expr);
	tree arg2_type = TREE_TYPE(arg2_expr);
	Instruction *arg1 = tree2inst_undefcheck(bb, arg1_expr);
	Instruction *arg2 = tree2inst_undefcheck(bb, arg2_expr);
	Instruction *cond;
	if (TREE_CODE(arg1_type) == COMPLEX_TYPE)
	  cond = process_binary_complex_cmp(code, arg1, arg2, boolean_type_node,
					    arg1_type, bb);
	else
	  cond = process_binary_scalar(code, arg1, arg2, boolean_type_node,
				       arg1_type, arg2_type, bb);
	edge true_edge, false_edge;
	extract_true_false_edges_from_block(gcc_bb, &true_edge, &false_edge);
	Basic_block *true_bb = gccbb2bb.at(true_edge->dest);
	Basic_block *false_bb = gccbb2bb.at(false_edge->dest);
	bb->build_br_inst(cond, true_bb, false_bb);
      }
    else
      {
	assert(EDGE_COUNT(gcc_bb->succs) == 1);
	Basic_block *succ_bb = gccbb2bb.at(single_succ_edge(gcc_bb)->dest);
	bb->build_br_inst(succ_bb);
      }
  }

  // We have created all instructions, so it is now safe to add the phi
  // arguments (as they must have been created now).
  for (int i = 0; i < nof_blocks; i++) {
    basic_block gcc_bb =
      (*fun->cfg->x_basic_block_info)[postorder[nof_blocks - 1 - i]];
    for (gphi_iterator gsi = gsi_start_phis(gcc_bb);
	 !gsi_end_p(gsi);
	 gsi_next(&gsi))
      {
	gphi *phi = gsi.phi();
	tree phi_result = gimple_phi_result(phi);
	if (VOID_TYPE_P(TREE_TYPE(phi_result)))
	  {
	    // Skip phi nodes for the memory SSA virtual SSA names.
	    continue;
	  }
	Instruction *phi_inst = tree2instruction.at(phi_result);
	Instruction *phi_undef = tree2undef.at(phi_result);
	for (unsigned i = 0; i < gimple_phi_num_args(phi); i++)
	  {
	    Basic_block *arg_bb = get_phi_arg_bb(phi, i);
	    tree arg = gimple_phi_arg_def(phi, i);
	    auto [arg_inst, arg_undef] = tree2inst(arg_bb, arg);
	    phi_inst->add_phi_arg(arg_inst, arg_bb);
	    if (!arg_undef)
	      arg_undef = arg_bb->value_inst(0, arg_inst->bitsize);
	    phi_undef->add_phi_arg(arg_undef, arg_bb);
	  }
      }
  }
}

Function *Converter::process_function()
{
  if (fun->static_chain_decl)
    {
      // TODO: Should be possible to handle this by treating it as a normal
      // pointer argument?
      throw Not_implemented("nested functions");
    }

  const char *name = function_name(fun);
  func = module->build_function(name);

  int *postorder = nullptr;
  try {
    postorder = XNEWVEC(int, last_basic_block_for_fn(fun));
    int nof_blocks = post_order_compute(postorder, true, true);

    // Build the new basic blocks.
    for (int i = nof_blocks - 1; i >= 0; --i) {
      basic_block gcc_bb = (*fun->cfg->x_basic_block_info)[postorder[i]];
      gccbb2bb[gcc_bb] = func->build_bb();
    }

    process_variables();
    process_func_args();
    process_instructions(nof_blocks, postorder);

    free(postorder);
    postorder = nullptr;
  }
  catch (...) {
    free(postorder);
    throw;
  }

  validate(func);

  // Some tests in the GCC test suite are far too large for the SMT solver
  // to handle. For example, gcc.c-torture/compile/20001226-1.c has 16390
  // basic blocks (after switch expansion) and gcc.dg/pr48141.c has a basic
  // block containing 3960038 instructions (after memory expansion).
  // It is useless to continue simplifying/checking such IR.
  //
  // Note: It would be more efficient to do these checks earlier, but I
  // want to build as much as possible in order to stress the converter.
  if (func->bbs.size() > MAX_BBS)
    throw Not_implemented("too many basic blocks");
  for (Basic_block *bb : func->bbs)
    {
      uint64_t nof_insts = 0;
      for (Instruction *inst = bb->first_inst; inst; inst = inst->next)
	{
	  if (++nof_insts > MAX_NOF_INSTS)
	    throw Not_implemented("too many instructions in a BB");
	}
    }

  reverse_post_order(func);
  simplify_insts(func);
  dead_code_elimination(func);
  simplify_cfg(func);
  if (loop_unroll(func))
    {
      simplify_insts(func);
      dead_code_elimination(func);
      simplify_cfg(func);
    }
  validate(func);

  Function *f = func;
  func = nullptr;
  return f;
}

}  // ennd empty namespace

Function *process_function(Module *module, CommonState *state, function *fun)
{
  Converter func(module, state, fun);
  return func.process_function();
}

Module *create_module()
{
  assert(POINTER_SIZE == 32 || POINTER_SIZE == 64);
  uint32_t ptr_bits;
  uint32_t ptr_id_bits;
  uint32_t ptr_offset_bits;
  if (POINTER_SIZE == 32)
    {
      ptr_bits = 32;
      ptr_id_bits = 8;
      ptr_offset_bits = 24;
    }
  else
    {
      ptr_bits = 64;
      ptr_id_bits = 16;
      ptr_offset_bits = 48;
    }
  return create_module(ptr_bits, ptr_id_bits, ptr_offset_bits);
}
