#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdlib>
#include <limits>
#include <set>
#include <sys/time.h>

#include "smtgcc.h"

namespace smtgcc {

const std::array<Instruction_info, 77> inst_info{{
  // Integer Comparison
  {"eq", Op::EQ, Inst_class::icomparison, true, true},
  {"ne", Op::NE, Inst_class::icomparison, true, true},
  {"sge", Op::SGE, Inst_class::icomparison, true, false},
  {"sgt", Op::SGT, Inst_class::icomparison, true, false},
  {"sle", Op::SLE, Inst_class::icomparison, true, false},
  {"slt", Op::SLT, Inst_class::icomparison, true, false},
  {"uge", Op::UGE, Inst_class::icomparison, true, false},
  {"ugt", Op::UGT, Inst_class::icomparison, true, false},
  {"ule", Op::ULE, Inst_class::icomparison, true, false},
  {"ult", Op::ULT, Inst_class::icomparison, true, false},

  // Floating-point comparison
  {"feq", Op::FEQ, Inst_class::fcomparison, true, true},
  {"fge", Op::FGE, Inst_class::fcomparison, true, false},
  {"fgt", Op::FGT, Inst_class::fcomparison, true, false},
  {"fle", Op::FLE, Inst_class::fcomparison, true, false},
  {"flt", Op::FLT, Inst_class::fcomparison, true, false},
  {"fne", Op::FNE, Inst_class::fcomparison, true, true},

  // Integer unary
  {"assert", Op::ASSERT, Inst_class::iunary, false, false},
  {"free", Op::FREE, Inst_class::iunary, false, false},
  {"get_mem_flag", Op::GET_MEM_FLAG, Inst_class::iunary, true, false},
  {"get_mem_undef", Op::GET_MEM_UNDEF, Inst_class::iunary, true, false},
  {"is_const_mem", Op::IS_CONST_MEM, Inst_class::iunary, true, false},
  {"is_noncanonical_nan", Op::IS_NONCANONICAL_NAN, Inst_class::iunary, true, false},
  {"load", Op::LOAD, Inst_class::iunary, true, false},
  {"mem_size", Op::MEM_SIZE, Inst_class::iunary, true, false},
  {"mov", Op::MOV, Inst_class::iunary, true, false},
  {"neg", Op::NEG, Inst_class::iunary, true, false},
  {"not", Op::NOT, Inst_class::iunary, true, false},
  {"read", Op::READ, Inst_class::iunary, true, false},
  {"register", Op::REGISTER, Inst_class::iunary, true, false},
  {"symbolic", Op::SYMBOLIC, Inst_class::iunary, true, false},
  {"ub", Op::UB, Inst_class::iunary, false, false},

  // Floating-point unary
  {"fabs", Op::FABS, Inst_class::funary, true, false},
  {"fneg", Op::FNEG, Inst_class::funary, true, false},

  // Integer binary
  {"add", Op::ADD, Inst_class::ibinary, true, true},
  {"and", Op::AND, Inst_class::ibinary, true, true},
  {"ashr", Op::ASHR, Inst_class::ibinary, true, false},
  {"concat", Op::CONCAT, Inst_class::ibinary, true, false},
  {"lshr", Op::LSHR, Inst_class::ibinary, true, false},
  {"mul", Op::MUL, Inst_class::ibinary, true, true},
  {"or", Op::OR, Inst_class::ibinary, true, true},
  {"param", Op::PARAM, Inst_class::ibinary, true, false},
  {"sadd_wraps", Op::SADD_WRAPS, Inst_class::ibinary, true, true},
  {"sdiv", Op::SDIV, Inst_class::ibinary, true, false},
  {"set_mem_flag", Op::SET_MEM_FLAG, Inst_class::ibinary, false, false},
  {"set_mem_undef", Op::SET_MEM_UNDEF, Inst_class::ibinary, false, false},
  {"shl", Op::SHL, Inst_class::ibinary, true, false},
  {"smax", Op::SMAX, Inst_class::ibinary, true, true},
  {"smin", Op::SMIN, Inst_class::ibinary, true, true},
  {"smul_wraps", Op::SMUL_WRAPS, Inst_class::ibinary, true, true},
  {"srem", Op::SREM, Inst_class::ibinary, true, false},
  {"ssub_wraps", Op::SSUB_WRAPS, Inst_class::ibinary, true, false},
  {"store", Op::STORE, Inst_class::ibinary, false, false},
  {"sub", Op::SUB, Inst_class::ibinary, true, false},
  {"udiv", Op::UDIV, Inst_class::ibinary, true, false},
  {"umax", Op::UMAX, Inst_class::ibinary, true, true},
  {"umin", Op::UMIN, Inst_class::ibinary, true, true},
  {"urem", Op::UREM, Inst_class::ibinary, true, false},
  {"write", Op::WRITE, Inst_class::ibinary, false, false},
  {"xor", Op::XOR, Inst_class::ibinary, true, true},

  // Floating-point binary
  {"fadd", Op::FADD, Inst_class::fbinary, true, true},
  {"fdiv", Op::FDIV, Inst_class::fbinary, true, false},
  {"fmul", Op::FMUL, Inst_class::fbinary, true, true},
  {"fsub", Op::FSUB, Inst_class::fbinary, true, false},

  // Ternary
  {"extract", Op::EXTRACT, Inst_class::ternary, true, false},
  {"ite", Op::ITE, Inst_class::ternary, true, false},
  {"memory", Op::MEMORY, Inst_class::ternary, true, false},

  // Conversions
  {"f2s", Op::F2S, Inst_class::conv, true, false},
  {"f2u", Op::F2U, Inst_class::conv, true, false},
  {"fchprec", Op::FCHPREC, Inst_class::conv, true, false},
  {"s2f", Op::S2F, Inst_class::conv, true, false},
  {"sext", Op::SEXT, Inst_class::conv, true, false},
  {"u2f", Op::U2F, Inst_class::conv, true, false},
  {"zext", Op::ZEXT, Inst_class::conv, true, false},

  // Special
  {"br", Op::BR, Inst_class::special, false, false},
  {"phi", Op::PHI, Inst_class::special, true, false},
  {"ret", Op::RET, Inst_class::special, false, false},
  {"value", Op::VALUE, Inst_class::special, true, false},
}};

#ifndef NDEBUG
// We are indexing into inst_info using the opcode. Validate that the
// array is sorted correctly.
struct Inst_info_validator
{
  Inst_info_validator()
  {
    for (unsigned i = 0; i < inst_info.size(); i++)
      {
	assert(i == (int)inst_info[i].opcode);
      }
  }
};
Inst_info_validator inst_info_validator;
#endif

Config::Config()
{
  verbose = 0;
  char *p = getenv("SMTGCC_VERBOSE");
  if (p)
    verbose = atoi(p);

  timeout = 120000;
  p = getenv("SMTGCC_TIMEOUT");
  if (p)
    timeout = atoi(p);

  memory_limit = 10 * 1024;
  p = getenv("SMTGCC_MEMORY_LIMIT");
  if (p)
    memory_limit = atoi(p);
}

Config config;

Instruction *create_inst(Op opcode, Instruction *arg)
{
  Instruction *inst = new Instruction;
  inst->opcode = opcode;
  inst->nof_args = 1;
  inst->arguments[0] = arg;
  if (opcode == Op::IS_CONST_MEM
      || opcode == Op::IS_NONCANONICAL_NAN
      || opcode == Op::GET_MEM_FLAG)
    inst->bitsize = 1;
  else if (opcode == Op::GET_MEM_UNDEF || opcode == Op::LOAD)
    inst->bitsize = 8;
  else if (opcode == Op::MEM_SIZE)
    inst->bitsize = arg->bb->func->module->ptr_offset_bits;
  else if (opcode == Op::SYMBOLIC || opcode == Op::REGISTER)
    inst->bitsize = arg->value();
  else if (opcode == Op::READ)
    {
      assert(arg->opcode == Op::REGISTER);
      inst->bitsize = arg->bitsize;
    }
  else
    inst->bitsize = arg->bitsize;
  return inst;
}

Instruction::Instruction()
{
  static uint32_t next_id = 0;
  id = next_id++;
}

Instruction *create_inst(Op opcode, Instruction *arg1, Instruction *arg2)
{
  Instruction *inst = new Instruction;
  inst->opcode = opcode;
  inst->nof_args = 2;
  inst->arguments[0] = arg1;
  inst->arguments[1] = arg2;
  Inst_class iclass = inst_info[(int)opcode].iclass;
  if (iclass == Inst_class::icomparison
      || iclass == Inst_class::fcomparison
      || opcode == Op::SADD_WRAPS
      || opcode == Op::SSUB_WRAPS
      || opcode == Op::SMUL_WRAPS)
    {
      assert(arg1->bitsize == arg2->bitsize);
      inst->bitsize = 1;
    }
  else if (iclass == Inst_class::conv)
    {
      inst->bitsize = arg2->value();
      if (opcode == Op::SEXT || opcode == Op::ZEXT)
	{
	  assert(inst->bitsize > arg1->bitsize);
	}
    }
  else if (opcode == Op::CONCAT)
    {
      inst->bitsize = arg1->bitsize + arg2->bitsize;
    }
  else if (opcode == Op::PARAM)
    {
      assert(arg1->opcode == Op::VALUE);
      assert(arg2->opcode == Op::VALUE);
      inst->bitsize = arg2->value();
    }
  else if (opcode == Op::STORE || opcode == Op::SET_MEM_UNDEF)
    {
      assert(arg1->bitsize == arg1->bb->func->module->ptr_bits);
      assert(arg2->bitsize == 8);
      inst->bitsize = 0;
    }
  else if (opcode == Op::SET_MEM_FLAG)
    {
      assert(arg1->bitsize == arg1->bb->func->module->ptr_bits);
      assert(arg2->bitsize == 1);
      inst->bitsize = 0;
    }
  else if (opcode == Op::WRITE)
    {
      assert(arg1->opcode == Op::REGISTER);
      inst->bitsize = 0;
    }
  else
    {
      assert(arg1->bitsize == arg2->bitsize);
      inst->bitsize = arg1->bitsize;
    }
  return inst;
}

Instruction *create_inst(Op opcode, Instruction *arg1, Instruction *arg2, Instruction *arg3)
{
  Instruction *inst = new Instruction;
  inst->opcode = opcode;
  inst->nof_args = 3;
  inst->arguments[0] = arg1;
  inst->arguments[1] = arg2;
  inst->arguments[2] = arg3;
  if (opcode == Op::EXTRACT)
    {
      uint32_t high = arg2->value();
      uint32_t low = arg3->value();
      assert(high >= low);
      assert(high < arg1->bitsize);
      inst->bitsize = 1 + high - low;
    }
  else if (opcode == Op::MEMORY)
    {
      assert(arg1->bitsize == arg1->bb->func->module->ptr_id_bits);
      assert(arg1->opcode == Op::VALUE);
      assert(arg2->bitsize == arg2->bb->func->module->ptr_offset_bits);
      assert(arg2->opcode == Op::VALUE);
      assert(arg3->opcode == Op::VALUE);
      inst->bitsize = arg1->bb->func->module->ptr_bits;
    }
  else
    {
      assert(opcode == Op::ITE);
      assert(arg1->bitsize == 1);
      assert(arg2->bitsize == arg3->bitsize);
      inst->bitsize = arg2->bitsize;
    }
  return inst;
}

Instruction *create_br_inst(Basic_block *dest_bb)
{
  Instruction *inst = new Instruction;
  inst->opcode = Op::BR;
  inst->u.br1.dest_bb = dest_bb;
  return inst;
}

Instruction *create_phi_inst(int bitsize)
{
  Instruction *inst = new Instruction;
  inst->opcode = Op::PHI;
  inst->bitsize = bitsize;
  return inst;
}

Instruction *Instruction::get_phi_arg(Basic_block *bb)
{
  auto it = std::find_if(phi_args.begin(), phi_args.end(), [bb](const Phi_arg& arg) {
    return arg.bb == bb;
  });
  assert(it != phi_args.end());
  return (*it).inst;
}

void Instruction::add_phi_arg(Instruction *inst, Basic_block *bb)
{
  Phi_arg phi_arg;
  phi_arg.inst = inst;
  phi_arg.bb = bb;
  this->phi_args.push_back(phi_arg);
  assert(inst->bitsize == this->bitsize);
  inst->used_by.insert(this);
}

void Instruction::remove_phi_arg(Basic_block *bb)
{
  auto it = std::find_if(phi_args.begin(), phi_args.end(), [bb](const Phi_arg& arg) {
    return arg.bb == bb;
  });
  assert(it != phi_args.end());
  Instruction *arg_inst = (*it).inst;
  phi_args.erase(it);

  // Remove this phi nodes from the arg_inst used_by if it is not used by
  // some other of the phi's arguments.
  it = std::find_if(phi_args.begin(), phi_args.end(), [arg_inst](const Phi_arg& arg) {
    return arg.inst == arg_inst;
  });
  if (it == phi_args.end())
    arg_inst->used_by.erase(this);
}

void Instruction::remove_phi_args()
{
  while (!phi_args.empty())
    {
      remove_phi_arg(phi_args.back().bb);
    }
}

void Instruction::print(FILE *stream) const
{
  fprintf(stream, "  ");
  if (has_lhs())
    fprintf(stream, "%%%" PRIu32 " = ", id);
  fprintf(stream, "%s", name());
  for (unsigned i = 0; i < nof_args; i++)
    {
      if (i == 0)
	fprintf(stream, " ");
      else
	fprintf(stream, ", ");
      fprintf(stream, "%%%" PRIu32, arguments[i]->id);
    }
  if (opcode == Op::BR)
    {
      if (nof_args == 0)
	fprintf(stream, " .%d", u.br1.dest_bb->id);
      else
	fprintf(stream, ", .%d, .%d", u.br3.true_bb->id,
		u.br3.false_bb->id);
    }
  else if (opcode == Op::VALUE)
    {
      uint64_t low = value();
      uint64_t high = value() >> 64;
      if (value() < 0x10000)
	fprintf(stream, " %" PRIu64 ", %" PRIu32, low, bitsize);
      else if (value() <= std::numeric_limits<uint32_t>::max())
	fprintf(stream, " 0x%08" PRIx64 ", %" PRIu32, low, bitsize);
      else if (value() <= std::numeric_limits<uint64_t>::max())
	fprintf(stream, " 0x%016" PRIx64 ", %" PRIu32, low, bitsize);
      else
	fprintf(stream, " 0x%016" PRIx64 "%016" PRIx64 ", %" PRIu32,
		high, low, bitsize);
    }
  else if (opcode == Op::PHI)
    {
      for (auto phi : phi_args)
	{
	  const char *s = (phi.bb != phi_args[0].bb) ? "," : "";
	  fprintf(stream, "%s [ %%%" PRIu32", .%d ]", s, phi.inst->id,
		  phi.bb->id);
	}
    }

  fprintf(stream, "\n");
}

Instruction *create_ret_inst()
{
  Instruction *inst = new Instruction;
  inst->opcode = Op::RET;
  inst->bitsize = 0;
  return inst;
}

Instruction *create_ret_inst(Instruction *arg)
{
  Instruction *inst = new Instruction;
  inst->opcode = Op::RET;
  inst->nof_args = 1;
  inst->arguments[0] = arg;
  inst->bitsize = arg->bitsize;
  return inst;
}

Instruction *create_ret_inst(Instruction *arg1, Instruction *arg2)
{
  assert(arg1->bitsize == arg2->bitsize);
  Instruction *inst = new Instruction;
  inst->opcode = Op::RET;
  inst->nof_args = 2;
  inst->arguments[0] = arg1;
  inst->arguments[1] = arg2;
  inst->bitsize = arg1->bitsize;
  return inst;
}

Instruction *create_br_inst(Instruction *cond, Basic_block *true_bb, Basic_block *false_bb)
{
  assert(true_bb != false_bb);
  Instruction *inst = new Instruction;
  inst->opcode = Op::BR;
  inst->nof_args = 1;
  inst->arguments[0] = cond;
  inst->u.br3.true_bb = true_bb;
  inst->u.br3.false_bb = false_bb;
  return inst;
}

unsigned __int128 Instruction::value() const
{
  assert(opcode == Op::VALUE);
  return u.value.value;
}

void Instruction::update_uses()
{
  assert(nof_args <= 3);
  if (nof_args > 0)
    arguments[0]->used_by.insert(this);
  if (nof_args > 1)
    arguments[1]->used_by.insert(this);
  if (nof_args > 2)
    arguments[2]->used_by.insert(this);
}

void Instruction::insert_after(Instruction *inst)
{
  // self.validate_unused()
  assert(!bb);
  assert(!prev);
  assert(!next);
  bb = inst->bb;
  update_uses();
  if (inst->next)
    inst->next->prev = this;
  next = inst->next;
  inst->next = this;
  prev = inst;
  if (inst == bb->last_inst)
    bb->last_inst = this;
}

void Instruction::insert_before(Instruction *inst)
{
  // self.validate_unused()
  assert(!bb);
  assert(!prev);
  assert(!next);
  bb = inst->bb;
  update_uses();
  if (inst->prev)
    inst->prev->next = this;
  prev = inst->prev;
  inst->prev = this;
  next = inst;
  if (inst == bb->first_inst)
    bb->first_inst = this;
}

void Instruction::move_before(Instruction *inst)
{
  assert(bb);
  assert(opcode != Op::PHI);
  assert(inst->opcode != Op::PHI);

  // Unlink the instruction from the BB.
  if (this == bb->first_inst)
    bb->first_inst = this->next;
  if (this == bb->last_inst)
    bb->last_inst = this->prev;
  if (this->prev)
    this->prev->next = this->next;
  if (this->next)
    inst->next->prev = this->prev;
  next = nullptr;
  prev = nullptr;
  bb = nullptr;

  insert_before(inst);
}

void Instruction::replace_use_with(Instruction *use, Instruction *new_inst)
{
  if (use->opcode == Op::PHI)
    {
      for (size_t i = 0; i < use->phi_args.size(); i++)
	{
	  if (use->phi_args[i].inst == this)
	    use->phi_args[i].inst = new_inst;
	}
    }
  else
    {
      for (size_t i = 0; i < use->nof_args; i++)
	{
	  if (use->arguments[i] == this)
	    use->arguments[i] = new_inst;
	}
    }
  new_inst->used_by.insert(use);

  auto I = std::find(used_by.begin(), used_by.end(), use);
  assert(I != used_by.end());
  used_by.erase(I);
}

void Instruction::replace_all_uses_with(Instruction *inst)
{
  for (Instruction *use : used_by)
    {
      if (use->opcode == Op::PHI)
	{
	  for (size_t i = 0; i < use->phi_args.size(); i++)
	    {
	      if (use->phi_args[i].inst == this)
		use->phi_args[i].inst = inst;
	    }
	}
      else
	{
	  for (size_t i = 0; i < use->nof_args; i++)
	    {
	      if (use->arguments[i] == this)
		use->arguments[i] = inst;
	    }
	}
      inst->used_by.insert(use);
    }
  used_by.clear();
}

// Insert the instruction at the last valid place in the basic block.
// Phi nodes are placed last in the list of phi nodes, even if there are
// already other instructions in the BB.
// Normal instructions are placed last in the BB, but before BR or RET.
void Basic_block::insert_last(Instruction *inst)
{
  assert(!inst->bb);
  assert(!inst->prev);
  assert(!inst->next);
  if (inst->opcode == Op::PHI)
    {
      insert_phi(inst);
      return;
    }
  if (inst->opcode == Op::BR)
    {
      assert(!last_inst ||
	     (last_inst->opcode != Op::BR
	      && last_inst->opcode != Op::RET));
      assert(succs.empty());
      if (inst->nof_args == 0)
	{
	  inst->u.br1.dest_bb->preds.push_back(this);
	  succs.push_back(inst->u.br1.dest_bb);
	}
      else
	{
	  assert(inst->nof_args == 1);
	  inst->u.br3.true_bb->preds.push_back(this);
	  succs.push_back(inst->u.br3.true_bb);
	  inst->u.br3.false_bb->preds.push_back(this);
	  succs.push_back(inst->u.br3.false_bb);
	}
    }
  else if (last_inst)
    {
      if (last_inst->opcode == Op::BR || last_inst->opcode == Op::RET)
	{
	  inst->insert_before(last_inst);
	  return;
	}
    }

  // self.validate_unused()
  inst->bb = this;
  inst->update_uses();
  if (last_inst)
    {
      inst->prev = last_inst;
      last_inst->next = inst;
    }
  last_inst = inst;
  if (!first_inst)
    first_inst = inst;
}

void Basic_block::insert_phi(Instruction *inst)
{
  assert(!inst->bb);
  assert(!inst->prev);
  assert(!inst->next);
  assert(inst->opcode == Op::PHI);
  phis.push_back(inst);
  inst->bb = this;
  inst->update_uses();
}

Instruction *Basic_block::build_inst(Op opcode, Instruction *arg)
{
  Instruction *inst = create_inst(opcode, arg);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_inst(Op opcode, Instruction *arg1, Instruction *arg2)
{
  Instruction *inst = create_inst(opcode, arg1, arg2);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_inst(Op opcode, Instruction *arg1, Instruction *arg2, Instruction *arg3)
{
  Instruction *inst = create_inst(opcode, arg1, arg2, arg3);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_phi_inst(int bitsize)
{
  Instruction *inst = create_phi_inst(bitsize);
  insert_phi(inst);
  return inst;
}

Instruction *Basic_block::build_ret_inst()
{
  Instruction *inst = create_ret_inst();
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_ret_inst(Instruction *arg)
{
  Instruction *inst = create_ret_inst(arg);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_ret_inst(Instruction *arg1, Instruction *arg2)
{
  Instruction *inst = create_ret_inst(arg1, arg2);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_br_inst(Basic_block *dest_bb)
{
  Instruction *inst = create_br_inst(dest_bb);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::build_br_inst(Instruction *cond, Basic_block *true_bb, Basic_block *false_bb)
{
  Instruction *inst = create_br_inst(cond, true_bb, false_bb);
  insert_last(inst);
  return inst;
}

Instruction *Basic_block::value_inst(unsigned __int128 value, uint32_t bitsize)
{
  return func->value_inst(value, bitsize);
}

Instruction *Basic_block::value_m1_inst(uint32_t bitsize)
{
  if (bitsize <= 128)
    return value_inst(-1, bitsize);

  Instruction *res = nullptr;
  while (bitsize)
    {
      uint32_t bs = std::min(bitsize, 128u);
      bitsize -= bs;
      Instruction *inst = value_inst(-1, bs);
      if (res)
	res = func->bbs[0]->build_inst(Op::CONCAT, inst, res);
      else
	res = inst;
    }
  return res;
}

Instruction *Basic_block::build_extract_id(Instruction *arg)
{
  assert(arg->bitsize == func->module->ptr_bits);
  Instruction *high = value_inst(func->module->ptr_id_high, 32);
  Instruction *low = value_inst(func->module->ptr_id_low, 32);
  return build_inst(Op::EXTRACT, arg, high, low);
}

Instruction *Basic_block::build_extract_offset(Instruction *arg)
{
  assert(arg->bitsize == func->module->ptr_bits);
  Instruction *high = value_inst(func->module->ptr_offset_high, 32);
  Instruction *low = value_inst(func->module->ptr_offset_low, 32);
  return build_inst(Op::EXTRACT, arg, high, low);
}

// Convenience function for extracting one bit.
// bit_idx = 0 is the lest significant bit.
Instruction *Basic_block::build_extract_bit(Instruction *arg, uint32_t bit_idx)
{
  assert(bit_idx < arg->bitsize);
  Instruction *idx = value_inst(bit_idx, 32);
  return build_inst(Op::EXTRACT, arg, idx, idx);
}

// Convenience function for truncating the value to nof_bits bits.
Instruction *Basic_block::build_trunc(Instruction *arg, uint32_t nof_bits)
{
  assert(nof_bits <= arg->bitsize);
  if (nof_bits == arg->bitsize)
    return arg;
  Instruction *high = value_inst(nof_bits - 1, 32);
  Instruction *low = value_inst(0, 32);
  return build_inst(Op::EXTRACT, arg, high, low);
}

void Basic_block::print(FILE *stream) const
{
  fprintf(stream, ".%d:\n", id);
  for (auto phi : phis)
    {
      phi->print(stream);
    }
  for (Instruction *inst = first_inst; inst; inst = inst->next)
    {
      inst->print(stream);
    }
}

Basic_block *Function::build_bb()
{
  Basic_block *bb = new Basic_block;
  bb->func = this;
  bb->id = next_bb_id++;
  bbs.push_back(bb);
  return bb;
}

Instruction *Function::value_inst(unsigned __int128 value, uint32_t bitsize)
{
  assert(bitsize > 0);

  if (bitsize < 128)
    value = (value << (128 - bitsize)) >> (128 - bitsize);
  auto key = std::pair(value, bitsize);
  auto it = values.find(key);
  if (it != values.end())
    return it->second;

  if (bitsize > 128)
    {
      Instruction *res = nullptr;
      while (bitsize)
	{
	  uint32_t bs = std::min(bitsize, 128u);
	  bitsize -= bs;
	  Instruction *inst = value_inst(value, bs);
	  value = 0;
	  if (res)
	    res = bbs[0]->build_inst(Op::CONCAT, inst, res);
	  else
	    res = inst;
	}
      // We do not insert the result in the values map since this is not
      // a real value instruction, so it will misbehave if the dead code
      // elimination pass removes it.
      return res;
    }

  Instruction *new_inst = new Instruction;
  new_inst->opcode = Op::VALUE;
  new_inst->u.value.value = value;
  new_inst->bitsize = bitsize;

  // We must insert the value instructions early in the basic block as they
  // may be used by e.g. memory initialization in the entry block.
  // But we want to preserve the order (so the IR generated when parsing
  // a file will be consistent with the original) so we cannot just insert
  // it at the top of the BB.
  if (!bbs[0]->last_inst || bbs[0]->last_inst->opcode == Op::VALUE)
    bbs[0]->insert_last(new_inst);
  else if (last_value_inst)
    new_inst->insert_after(last_value_inst);
  else
    {
      Instruction *inst = bbs[0]->first_inst;
      while (inst && inst->opcode == Op::VALUE)
	{
	  inst = inst->next;
	}
      if (inst)
	new_inst->insert_before(inst);
      else
	bbs[0]->insert_last(new_inst);
    }
  last_value_inst = new_inst;
  values[key] = new_inst;
  return new_inst;
}

void Function::rename(const std::string& str)
{
  name = str;
}

void Function::canonicalize()
{
  reset_ir_id();

  // Sort arguments for commutative instructions so that the argument with
  // lowerest ID is first. This speeds up Z3 verification (for cases where
  // GCC passes, such as ccp, has made pointless argument swaps) as it then
  // apparently easier can see that the result are identical, and can prune
  // that part of the search space.
  for (Basic_block *bb : bbs)
    {
      for (Instruction *inst = bb->first_inst; inst; inst = inst->next)
	{
	  if (inst->is_commutative())
	    {
	      assert(inst->nof_args == 2);
	      if (inst->arguments[0]->id > inst->arguments[1]->id)
		std::swap(inst->arguments[0], inst->arguments[1]);
	    }
	}

      // The code generating SMT2 assumes the phi nodes and BB preds are
      // sorted in reverse post order.
      for (auto phi : bb->phis)
	{
	  std::sort(phi->phi_args.begin(), phi->phi_args.end(),
		    [](const Phi_arg &a, const Phi_arg &b) {
		      return a.bb->id < b.bb->id;
		    });
	}
      std::sort(bb->preds.begin(), bb->preds.end(),
		[](const Basic_block *a, const Basic_block *b) {
		  return a->id < b->id;
		});
    }
}

void Function::reset_ir_id()
{
  int bb_nbr = 0;
  uint32_t inst_nbr = 0;
  for (Basic_block *bb : bbs)
    {
      bb->id = bb_nbr++;
      for (Instruction *phi : bb->phis)
	{
	  phi->id = inst_nbr++;
	}
      for (Instruction *inst = bb->first_inst; inst; inst = inst->next)
	{
	  inst->id = inst_nbr++;
	}
    }
}

void Function::print(FILE *stream) const
{
  fprintf(stream, "\nfunction %s\n", name.c_str());
  for (auto bb : bbs)
    {
      if (bb != bbs[0])
	fprintf(stream, "\n");
      bb->print(stream);
    }
}

Function *Module::build_function(const std::string& name)
{
  Function *func = new Function;
  func->module = this;
  func->name = name;
  functions.push_back(func);
  return func;
}

void Module::print(FILE *stream) const
{
  fprintf(stream, "config %" PRIu32 ", %" PRIu32 ", %" PRIu32 "\n",
	  ptr_bits, ptr_id_bits, ptr_offset_bits);

  for (auto func : functions)
    func->print(stream);
}

Module *create_module(uint32_t ptr_bits, uint32_t ptr_id_bits, uint32_t ptr_offset_bits)
{
  assert(ptr_bits == 32 || ptr_bits == 64);
  assert(ptr_bits == ptr_id_bits + ptr_offset_bits);
  Module *module = new Module;
  module->ptr_bits = ptr_bits;
  module->ptr_offset_bits = ptr_offset_bits;
  module->ptr_offset_low = 0;
  module->ptr_offset_high = ptr_offset_bits - 1;
  module->ptr_id_bits = ptr_id_bits;
  module->ptr_id_low = ptr_offset_bits;
  module->ptr_id_high = ptr_offset_bits + ptr_id_bits - 1;
  return module;
}

void destroy_module(struct Module *module)
{
  while (!module->functions.empty())
    destroy_function(module->functions[0]);
  delete module;
}

void destroy_function(Function *func)
{
  // The functions destroying basic blocks and instructions does extra work
  // preserving function invariants (as they are meant to be used by
  // optimization passes etc.). This is not needed when destroying the
  // function, so we'll just delete them.
  for (Basic_block *bb : func->bbs)
    {
      for (Instruction *inst : bb->phis)
	delete inst;
      Instruction *next_inst = bb->first_inst;
      while (next_inst)
	{
	  Instruction *inst = next_inst;
	  next_inst = next_inst->next;
	  delete inst;
	}
      delete bb;
    }

  // Unlink func from module.
  Module *module = func->module;
  auto I = std::find(module->functions.begin(), module->functions.end(), func);
  if (I != module->functions.end())
    module->functions.erase(I);

  delete func;
}

void destroy_basic_block(Basic_block *bb)
{
  // Pointers from predecessor will be dangling after we destroy the BB.
  assert(bb->preds.empty());

  for (Instruction *phi : bb->phis)
    {
      phi->remove_phi_args();
    }
  for (Instruction *inst = bb->last_inst; inst;)
    {
      Instruction *curr_inst = inst;
      inst = inst->prev;
      destroy_instruction(curr_inst);
    }
  while (!bb->phis.empty())
    {
      destroy_instruction(bb->phis.back());
    }
  auto it = std::find(bb->func->bbs.begin(), bb->func->bbs.end(), bb);
  assert(it != bb->func->bbs.end());
  bb->func->bbs.erase(it);
  delete bb;
}

void destroy_instruction(Instruction *inst)
{
  assert(inst->used_by.empty());

  if (inst->bb)
    {
      if (inst->opcode == Op::VALUE)
	{
	  auto key = std::pair(inst->value(), inst->bitsize);
	  assert(inst->bb->func->values.contains(key));
	  inst->bb->func->values.erase(key);

	  if (inst->bb->func->last_value_inst == inst)
	    {
	      Instruction *prev = inst->prev;
	      if (prev && prev->opcode == Op::VALUE)
		inst->bb->func->last_value_inst = prev;
	      else
		inst->bb->func->last_value_inst = nullptr;
	    }
	}

      Basic_block *bb = inst->bb;
      if (inst->opcode == Op::PHI)
	{
	  for (auto [arg_inst, arg_bb] : inst->phi_args)
	    {
	      arg_inst->used_by.erase(inst);
	    }

	  auto it = std::find(bb->phis.begin(), bb->phis.end(), inst);
	  assert(it != bb->phis.end());
	  bb->phis.erase(it);
	}
      else
	{
	  if (inst->opcode == Op::BR)
	    {
	      for (auto succ : inst->bb->succs)
		{
		  auto it = find(succ->preds.begin(), succ->preds.end(), inst->bb);
		  assert(it != succ->preds.end());
		  succ->preds.erase(it);
		}
	      inst->bb->succs.clear();
	      // Note: phi instructions in the successor basic blocks
	      // will have arguments for the now removed branches.
	      // But we cannot fix that here as the reason the branch is
	      // removed may be because the caller want to add a new, similar,
	      // branch, and removing data from the phi nodes will make
	      // that work harder. So it is up to the caller to update
	      // phi nodes as appropriate.
	    }

	  for (uint64_t i = 0; i < inst->nof_args; i++)
	    {
	      inst->arguments[i]->used_by.erase(inst);
	    }

	  if (inst == bb->first_inst)
	    bb->first_inst = inst->next;
	  if (inst == bb->last_inst)
	    bb->last_inst = inst->prev;
	  if (inst->prev)
	    inst->prev->next = inst->next;
	  if (inst->next)
	    inst->next->prev = inst->prev;
	}
    }
  delete inst;
}

bool identical(Instruction *inst1, Instruction *inst2)
{
  if (inst1->opcode != inst2->opcode)
    return false;
  if (inst1->opcode == Op::SYMBOLIC)
    {
      // The SYMBOLIC instruction represents "all" values, and we must handle
      // that the inst1 and inst2 get different concrete values.
      return false;
    }
  if (inst1->bitsize != inst2->bitsize)
    return false;
  if (inst1->nof_args != inst2->nof_args)
    return false;
  if (inst1->is_commutative())
    {
      // Some passes (such as ccp) may do pointless argument swaps.
      assert(inst1->nof_args == 2);
      int nbr1_0 = inst1->arguments[0]->id;
      int nbr1_1 = inst1->arguments[1]->id;
      int nbr2_0 = inst2->arguments[0]->id;
      int nbr2_1 = inst2->arguments[1]->id;
      if (!((nbr1_0 == nbr2_0 && nbr1_1 == nbr2_1)
	    || (nbr1_0 == nbr2_1 &&  nbr1_1 == nbr2_0)))
	return false;
    }
  else
    for (size_t i = 0; i < inst1->nof_args; i++)
      {
	if (inst1->arguments[i]->id != inst2->arguments[i]->id)
	  return false;
      }

  // The normal instructions are fully checked by the above, but the
  // instructions of class "special" need some additional checks.
  switch (inst1->opcode)
    {
    case Op::BR:
      if (inst1->nof_args == 0)
	{
	  if (inst1->u.br1.dest_bb->id != inst2->u.br1.dest_bb->id)
	    return false;
	}
      else
	{
	  if (inst1->u.br3.true_bb->id != inst2->u.br3.true_bb->id)
	    return false;
	  if (inst1->u.br3.false_bb->id != inst2->u.br3.false_bb->id)
	    return false;
	}
      break;
    case Op::PHI:
      if (inst1->phi_args.size() != inst2->phi_args.size())
	return false;
      for (size_t i = 0; i < inst1->phi_args.size(); i++)
	{
	  Phi_arg arg1 = inst1->phi_args[i];
	  Phi_arg arg2 = inst2->phi_args[i];
	  if (arg1.inst->id != arg2.inst->id)
	    return false;
	  if (arg1.bb->id != arg2.bb->id)
	    return false;
	}
      break;
    case Op::RET:
      // This is already checked by the argument check above.
      break;
    case Op::VALUE:
      if (inst1->value() != inst2->value())
	return false;
      break;
    default:
      // If this is an instruction of class "special", then there is a missing
      // case in this switch statement.
      assert(inst1->iclass() != Inst_class::special);
      break;
    }

  return true;
}

bool identical(Function *func1, Function *func2)
{
  func1->canonicalize();
  func2->canonicalize();

  if (func1->bbs.size() != func2->bbs.size())
    return false;

  for (size_t i = 0; i < func1->bbs.size(); i++)
    {
      Basic_block *bb1 = func1->bbs[i];
      Basic_block *bb2 = func2->bbs[i];
      if (bb1->phis.size() != bb2->phis.size())
	return false;
      for (size_t j = 0; j < bb1->phis.size(); j++)
	{
	  Instruction *phi1 = bb1->phis[j];
	  Instruction *phi2 = bb2->phis[j];
	  if (!identical(phi1, phi2))
	    return false;
	}
      Instruction *inst1 = bb1->first_inst;
      Instruction *inst2 = bb2->first_inst;
      while (inst1 && inst2)
	{
	  if (!identical(inst1, inst2))
	    return false;
	  inst1 = inst1->next;
	  inst2 = inst2->next;
	}
      if (inst1 || inst2)
	return false;
    }

  return true;
}

uint64_t get_time()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

std::optional<std::string> check_refine(Module *module)
{
  struct VStats {
    SStats cvc5;
    SStats z3;
  } stats;

  assert(module->functions.size() == 2);
  Function *src = module->functions[0];
  Function *tgt = module->functions[1];
  if (src->name != "src")
    std::swap(src, tgt);
  assert(src->name == "src" && tgt->name == "tgt");

  if (identical(src, tgt))
    return {};

  if (config.verbose > 1)
    module->print(stderr);

  std::optional<std::string> msg;
#if 0
  auto [stats_cvc5, msg_cvc5] = check_refine_cvc5(src, tgt);
  stats.cvc5 = stats_cvc5;
  if (msg_cvc5)
    msg = msg_cvc5;
#endif
#if 1
  auto [stats_z3, msg_z3] = check_refine_z3(src, tgt);
  stats.z3 = stats_z3;
  if (msg_z3)
    msg = msg_z3;
#endif

  if (config.verbose > 0)
    {
      if (!stats.cvc5.skipped || !stats.z3.skipped)
	{
	  fprintf(stderr, "SMTGCC: time: ");
	  for (int i = 0; i < 3; i++)
	    {
	      fprintf(stderr, "%s%" PRIu64, i ? "," : "", stats.cvc5.time[i]);
	    }
	  for (int i = 0; i < 3; i++)
	    {
	      fprintf(stderr, "%s%" PRIu64, ",", stats.z3.time[i]);
	    }
	  fprintf(stderr, "\n");
	}
    }

  return msg;
}

std::optional<std::string> check_ub(Function *func)
{
  struct VStats {
    SStats cvc5;
    SStats z3;
  } stats;

  if (config.verbose > 1)
    func->print(stderr);

  std::optional<std::string> msg;
#if 0
  auto [stats_cvc5, msg_cvc5] = check_ub_cvc5(func);
  stats.cvc5 = stats_cvc5;
  if (msg_cvc5)
    msg = msg_cvc5;
#endif
#if 1
  auto [stats_z3, msg_z3] = check_ub_z3(func);
  stats.z3 = stats_z3;
  if (msg_z3)
    msg = msg_z3;
#endif

  if (config.verbose > 0)
    {
      if (!stats.cvc5.skipped || !stats.z3.skipped)
	{
	  fprintf(stderr, "SMTGCC: time: ");
	  for (int i = 0; i < 3; i++)
	    {
	      fprintf(stderr, "%s%" PRIu64, i ? "," : "", stats.cvc5.time[i]);
	    }
	  for (int i = 0; i < 3; i++)
	    {
	      fprintf(stderr, "%s%" PRIu64, ",", stats.z3.time[i]);
	    }
	  fprintf(stderr, "\n");
	}
    }

  return msg;
}

std::optional<std::string> check_assert(Function *func)
{
  struct VStats {
    SStats cvc5;
    SStats z3;
  } stats;

  if (config.verbose > 1)
    func->print(stderr);

  std::optional<std::string> msg;
#if 0
  auto [stats_cvc5, msg_cvc5] = check_assert_cvc5(func);
  stats.cvc5 = stats_cvc5;
  if (msg_cvc5)
    msg = msg_cvc5;
#endif
#if 1
  auto [stats_z3, msg_z3] = check_assert_z3(func);
  stats.z3 = stats_z3;
  if (msg_z3)
    msg = msg_z3;
#endif

  if (config.verbose > 0)
    {
      if (!stats.cvc5.skipped || !stats.z3.skipped)
	{
	  fprintf(stderr, "SMTGCC: time: ");
	  for (int i = 0; i < 3; i++)
	    {
	      fprintf(stderr, "%s%" PRIu64, i ? "," : "", stats.cvc5.time[i]);
	    }
	  for (int i = 0; i < 3; i++)
	    {
	      fprintf(stderr, "%s%" PRIu64, ",", stats.z3.time[i]);
	    }
	  fprintf(stderr, "\n");
	}
    }

  return msg;
}

} // end namespace smtgcc