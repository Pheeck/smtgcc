#include <fstream>
#include <cassert>
#include <limits>
#include <string>

#include "smtgcc.h"

#define BITSIZE 64

using namespace std::string_literals;

using namespace smtgcc;

namespace smtgcc {
namespace {

// TODO: Check that all instructions are supported by asm. For example,
// I am not sure that the "w" version of sgt is supported...

struct parser {
  enum class lexeme {
    label,
    label_def,
    variable,
    name,
    integer,
    hex,
    comma,
    assign,
    left_bracket,
    right_bracket
  };

  struct token {
    lexeme kind;
    int pos;
    int size;
  };
  std::vector<token> tokens;
  std::vector<Instruction *> registers;
  std::vector<Basic_block *> ret_bbs;

  int line_number = 0;
  int pos;

  static const int max_line_len = 1000;
  char buf[max_line_len];

  Function *parse(std::string const& file_name, riscv_state *state);

  Module *modul;
  Function *src_func;

private:
  Function *current_func = nullptr;
  Basic_block *current_bb = nullptr;
  std::map<uint32_t, Basic_block *> id2bb;
  std::map<Basic_block *, uint32_t> bb2id;
  std::map<uint32_t, Instruction *> id2inst;

  void lex_line(void);
  void lex_label_or_label_def(void);
  void lex_hex(void);
  void lex_integer(void);
  void lex_hex_or_integer(void);
  void lex_name(void);

  std::string token_string(const token& tok);

  std::string get_name(const char *p);
  uint32_t get_u32(const char *p);
  unsigned __int128 get_hex(const char *p);

  unsigned __int128 get_hex_or_integer(unsigned idx);
  Instruction *get_reg(unsigned idx);
  Instruction *get_imm(unsigned idx);
  Instruction *get_reg_value(unsigned idx);
  Basic_block *get_bb(unsigned idx);
  Basic_block *get_bb_def(unsigned idx);
  void get_comma(unsigned idx);
  void get_end_of_line(unsigned idx);
  void gen_cond_branch(Op opcode);

  void parse_function();

  void skip_space_and_comments();
};

void parser::skip_space_and_comments()
{
  while (isspace(buf[pos]))
    pos++;
  if (buf[pos] == ';')
    {
      while (buf[pos])
	pos++;
    }
}

void parser::lex_label_or_label_def(void)
{
  assert(buf[pos] == '.');
  int start_pos = pos;
  pos++;
  if (buf[pos] != 'L')
    throw Parse_error("expected 'L' after '.'", line_number);
  pos++;
  if (!isdigit(buf[pos]))
    throw Parse_error("expected a digit after \".L\"", line_number);
  pos++;
  if (isdigit(buf[pos]) && buf[pos - 1] == '0')
    throw Parse_error("octal numbers are not supported in labels", line_number);
  while (isdigit(buf[pos]))
    pos++;
  if (buf[pos] == ':')
    {
      pos++;
      tokens.emplace_back(lexeme::label_def, start_pos, pos - start_pos);
    }
  else
    tokens.emplace_back(lexeme::label, start_pos, pos - start_pos);
}

void parser::lex_hex(void)
{
  assert(buf[pos] == '0');
  int start_pos = pos;
  pos++;
  assert(buf[pos] == 'x' || buf[pos] == 'X');
  pos++;
  if (!isxdigit(buf[pos]))
    throw Parse_error("expected a hex digit after 0x", line_number);
  while (isxdigit(buf[pos]))
    pos++;
  tokens.emplace_back(lexeme::hex, start_pos, pos - start_pos);
}

void parser::lex_integer(void)
{
  int start_pos = pos;
  if (buf[pos] == '-')
    pos++;
  assert(isdigit(buf[pos]));
  pos++;
  if (isdigit(buf[pos]) && buf[pos - 1] == '0')
    throw Parse_error("octal numbers are not supported", line_number);
  while (isdigit(buf[pos]))
    pos++;
  tokens.emplace_back(lexeme::integer, start_pos, pos - start_pos);
}

void parser::lex_hex_or_integer(void)
{
  assert(isdigit(buf[pos]) || buf[pos] == '-');
  if (buf[pos] == '0' && (buf[pos + 1] == 'x' || buf[pos + 1] == 'X'))
    lex_hex();
  else
    lex_integer();
}

void parser::lex_name(void)
{
  assert(isalpha(buf[pos]) || buf[pos] == '_' || buf[pos] == '.');
  int start_pos = pos;
  pos++;
  while (isalnum(buf[pos]) || buf[pos] == '_' || buf[pos] == '-' || buf[pos] == '.')
    pos++;
  tokens.emplace_back(lexeme::name, start_pos, pos - start_pos);
}

std::string parser::token_string(const token& tok)
{
  return std::string(&buf[tok.pos], tok.size);
}

std::string parser::get_name(const char *p)
{
  std::string name;
  while (isalnum(*p) || *p == '_' || *p == '-' || *p == '.')
    name.push_back(*p++);
  return name;
}

uint32_t parser::get_u32(const char *p)
{
  assert(isdigit(*p));
  uint64_t value = 0;
  while (isdigit(*p))
    {
      value = value * 10 + (*p++ - '0');
      if (value > std::numeric_limits<uint32_t>::max())
	throw Parse_error("too large decimal integer value", line_number);
    }
  return value;
}

unsigned __int128 parser::get_hex(const char *p)
{
  const unsigned __int128 max_val = -1;
  unsigned __int128 value = 0;
  p += 2;
  while (isxdigit(*p))
    {
      if (value > (max_val >> 4))
	throw Parse_error("too large hexadecimal value", line_number);
      unsigned nibble;
      if (isdigit(*p))
	nibble = *p - '0';
      else if (isupper(*p))
	nibble = 10 + (*p - 'A');
      else
	nibble = 10 + (*p - 'a');
      value = (value << 4) | nibble;
      p++;
    }
  return value;
}

unsigned __int128 parser::get_hex_or_integer(unsigned idx)
{
  if (tokens.size() <= idx)
    throw Parse_error("expected more arguments", line_number);
  if (tokens[idx].kind != lexeme::hex && tokens[idx].kind != lexeme::integer)
    throw Parse_error("expected a hexadecimal or decimal integer instead of "
		      + token_string(tokens[idx]), line_number);

  int pos = tokens[idx].pos;
  if (buf[pos] == '-')
    pos++;
  unsigned __int128 val;
  if (tokens[idx].kind == lexeme::integer)
    val = get_u32(&buf[pos]);
  else
    val = get_hex(&buf[pos]);
  if (buf[tokens[idx].pos] == '-')
    val = -val;
  return val;
}

Instruction *parser::get_reg(unsigned idx)
{
  if (tokens.size() <= idx)
    throw Parse_error("expected more arguments", line_number);
  if (tokens[idx].kind != lexeme::name || (buf[tokens[idx].pos] != 'a' && buf[tokens[idx].pos] != 't'))
    throw Parse_error("expected a register instead of "
		      + token_string(tokens[idx]), line_number);
  // TODO: Check length.
  uint32_t value = buf[tokens[idx].pos + 1] - '0';
  if (tokens[idx].size == 3)
    value = value * 10 + (buf[tokens[idx].pos + 1] - '0');
  if (buf[tokens[idx].pos] == 'a')
    return registers[10 + value];
  else if (buf[tokens[idx].pos] == 't')
    {
      if (value < 3)
	return registers[5 + value];
      else
	return registers[28 - 3 + value];
    }
  else
    throw Parse_error("expected a register instead of "
		      + token_string(tokens[idx]), line_number);
}

Instruction *parser::get_imm(unsigned idx)
{
  uint32_t value = get_hex_or_integer(idx);
  Instruction *inst =  current_bb->value_inst(value, 12);
  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
  return current_bb->build_inst(Op::SEXT, inst, bitsize);
}

Instruction *parser::get_reg_value(unsigned idx)
{
  if (tokens.size() <= idx)
    throw Parse_error("expected more arguments", line_number);
  if (tokens[idx].size == 4
      && buf[tokens[idx].pos + 0] == 'z'
      && buf[tokens[idx].pos + 1] == 'e'
      && buf[tokens[idx].pos + 2] == 'r'
      && buf[tokens[idx].pos + 3] == 'o')
    return current_bb->value_inst(0, BITSIZE);
  if (tokens[idx].kind != lexeme::name || (buf[tokens[idx].pos] != 'a' && buf[tokens[idx].pos] != 't'))
    throw Parse_error("expected a register instead of "
		      + token_string(tokens[idx]), line_number);
  // TODO: Check length.
  uint32_t value = buf[tokens[idx].pos + 1] - '0';
  if (tokens[idx].size == 3)
    value = value * 10 + (buf[tokens[idx].pos + 1] - '0');
  if (buf[tokens[idx].pos] == 'a')
    return current_bb->build_inst(Op::READ, registers[10 + value]);
  else if (buf[tokens[idx].pos] == 't')
    {
      if (value < 3)
	return current_bb->build_inst(Op::READ, registers[5 + value]);
      else
	return current_bb->build_inst(Op::READ, registers[28 - 3 + value]);
    }
  else
    throw Parse_error("expected a register instead of "
		      + token_string(tokens[idx]), line_number);
}

Basic_block *parser::get_bb(unsigned idx)
{
  if (tokens.size() <= idx)
    throw Parse_error("expected more arguments", line_number);
 if (tokens[idx].kind != lexeme::label)
   throw Parse_error("expected a label instead of "
		     + token_string(tokens[idx]), line_number);
  uint32_t id = get_u32(&buf[tokens[idx].pos + 2]);
  auto I = id2bb.find(id);
  if (I != id2bb.end())
    return I->second;
  Basic_block *bb = current_func->build_bb();
  id2bb[id] = bb;
  bb2id[bb] = id;
  return bb;
}

Basic_block *parser::get_bb_def(unsigned idx)
{
  if (tokens.size() <= idx)
    throw Parse_error("expected more arguments", line_number);
 if (tokens[idx].kind != lexeme::label_def)
   throw Parse_error("expected a label instead of "
		     + token_string(tokens[idx]), line_number);
  uint32_t id = get_u32(&buf[tokens[idx].pos + 2]);
  auto I = id2bb.find(id);
  if (I != id2bb.end())
    return I->second;
  Basic_block *bb = current_func->build_bb();
  id2bb[id] = bb;
  bb2id[bb] = id;
  return bb;
}

void parser::get_comma(unsigned idx)
{
  assert(idx > 0);
  if (tokens.size() <= idx || tokens[idx].kind != lexeme::comma)
    throw Parse_error("expected a ',' after " + token_string(tokens[idx - 1]),
		      line_number);
}

void parser::get_end_of_line(unsigned idx)
{
  assert(idx > 0);
  if (tokens.size() > idx)
    throw Parse_error("expected end of line after " +
		      token_string(tokens[idx - 1]), line_number);
}

void parser::gen_cond_branch(Op opcode)
{
  Instruction *arg1 = get_reg_value(1);
  get_comma(2);
  Instruction *arg2 = get_reg_value(3);
  get_comma(4);
  Basic_block *true_bb = get_bb(5);
  get_end_of_line(6);

  Basic_block *false_bb = current_func->build_bb();
  Instruction *cond = current_bb->build_inst(opcode, arg1, arg2);
  current_bb->build_br_inst(cond, true_bb, false_bb);
  current_bb = false_bb;
}

void parser::parse_function()
{
  if (tokens[0].kind == lexeme::label_def)
  {
    Basic_block *bb = get_bb_def(0);
    get_end_of_line(1);

    if (current_bb)
      current_bb->build_br_inst(bb);
    current_bb = bb;
    return;
  }

  std::string name = get_name(&buf[tokens[0].pos]);
  if (name == "add" || name == "addw" || name == "addi" || name == "addiw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "addi" || name == "addiw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "addw" || name == "addiw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::ADD, arg1, arg2);
      if (name == "addw" || name == "addiw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "mul" || name == "mulw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "mulw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::MUL, arg1, arg2);
      if (name == "mulw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "div" || name == "divw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "divw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::SDIV, arg1, arg2);
      if (name == "divw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "divu" || name == "divuw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "divuw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::UDIV, arg1, arg2);
      if (name == "divuw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "rem" || name == "remw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "remw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::SREM, arg1, arg2);
      if (name == "remw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "remu" || name == "remuw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "remuw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::UREM, arg1, arg2);
      if (name == "remuw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "slt" || name == "sltw" || name == "slti" || name == "sltiw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "slti" || name == "sltiw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "sltw" || name == "sltiw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::SLT, arg1, arg2);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::ZEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sltu" || name == "sltuw"
	   || name == "sltiu" || name == "sltiuw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "sltiu" || name == "sltiuw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "sltuw" || name == "sltiuw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::ULT, arg1, arg2);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::ZEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sgt" || name == "sgtw")
    {
      // Pseudo instruction.
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "sgtuw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::SGT, arg1, arg2);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::ZEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sgtu" || name == "sgtuw")
    {
      // Pseudo instruction.
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "sgtuw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::UGT, arg1, arg2);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::ZEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "seqz" || name == "seqzw")
    {
      // Pseudo instruction.
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_end_of_line(4);

      if (name == "seqzw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	}
      Instruction *zero = current_bb->value_inst(0, arg1->bitsize);
      Instruction *res = current_bb->build_inst(Op::EQ, arg1, zero);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::ZEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "snez" || name == "snezw")
    {
      // Pseudo instruction.
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_end_of_line(4);

      if (name == "snezw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	}
      Instruction *zero = current_bb->value_inst(0, arg1->bitsize);
      Instruction *res = current_bb->build_inst(Op::NE, arg1, zero);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::ZEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "and" || name == "andw" || name == "andi" || name == "andiw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "andi" || name == "andiw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "andw" || name == "andiw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::AND, arg1, arg2);
      if (name == "andw" || name == "andiw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "or" || name == "orw" || name == "ori" || name == "oriw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "ori" || name == "oriw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "orw" || name == "oriw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::OR, arg1, arg2);
      if (name == "orw" || name == "oriw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "xor" || name == "xorw" || name == "xori" || name == "xoriw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "xori" || name == "xoriw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "xorw" || name == "xoriw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::XOR, arg1, arg2);
      if (name == "xorw" || name == "xoriw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sll" || name == "sllw" || name == "slli" || name == "slliw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "slli" || name == "slliw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (BITSIZE == 32 || name == "sllw" || name == "slliw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 5);
	  Instruction *bitsize = current_bb->value_inst(32, 32);
	  arg2 = current_bb->build_inst(Op::ZEXT, arg2, bitsize);
	}
      else
	{
	  arg2 = current_bb->build_trunc(arg2, 6);
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  arg2 = current_bb->build_inst(Op::ZEXT, arg2, bitsize);
	}
      Instruction *res = current_bb->build_inst(Op::SHL, arg1, arg2);
      if (name == "sllw" || name == "slliw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "srl" || name == "srlw" || name == "srli" || name == "srliw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "srli" || name == "srliw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (BITSIZE == 32 || name == "srlw" || name == "srliw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 5);
	  Instruction *bitsize = current_bb->value_inst(32, 32);
	  arg2 = current_bb->build_inst(Op::ZEXT, arg2, bitsize);
	}
      else
	{
	  arg2 = current_bb->build_trunc(arg2, 6);
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  arg2 = current_bb->build_inst(Op::ZEXT, arg2, bitsize);
	}
      Instruction *res = current_bb->build_inst(Op::LSHR, arg1, arg2);
      if (name == "srlw" || name == "srliw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sra" || name == "sraw" || name == "srai" || name == "sraiw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2;
      if (name == "srai" || name == "sraiw")
	arg2 = get_imm(5);
      else
	arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (BITSIZE == 32 || name == "sraw" || name == "sraiw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 5);
	  Instruction *bitsize = current_bb->value_inst(32, 32);
	  arg2 = current_bb->build_inst(Op::ZEXT, arg2, bitsize);
	}
      else
	{
	  arg2 = current_bb->build_trunc(arg2, 6);
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  arg2 = current_bb->build_inst(Op::ZEXT, arg2, bitsize);
	}
      Instruction *res = current_bb->build_inst(Op::ASHR, arg1, arg2);
      if (name == "sraw" || name == "sraiw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sub" || name == "subw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_comma(4);
      Instruction *arg2 = get_reg_value(5);
      get_end_of_line(6);

      if (name == "subw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	  arg2 = current_bb->build_trunc(arg2, 32);
	}
      Instruction *res = current_bb->build_inst(Op::SUB, arg1, arg2);
      if (name == "subw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "neg" || name == "negw")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_end_of_line(4);

      if (name == "negw")
	{
	  arg1 = current_bb->build_trunc(arg1, 32);
	}
      Instruction *res = current_bb->build_inst(Op::NEG, arg1);
      if (name == "negw")
	{
	  Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
	  res = current_bb->build_inst(Op::SEXT, res, bitsize);
	}
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "sext.w")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_end_of_line(4);

      Instruction *res = current_bb->build_trunc(arg1, 32);
      Instruction *bitsize = current_bb->value_inst(BITSIZE, 32);
      res = current_bb->build_inst(Op::SEXT, res, bitsize);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "not")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_end_of_line(4);

      Instruction *res = current_bb->build_inst(Op::NOT, arg1);
      current_bb->build_inst(Op::WRITE, dest, res);
    }
  else if (name == "mv")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      Instruction *arg1 = get_reg_value(3);
      get_end_of_line(4);

      current_bb->build_inst(Op::WRITE, dest, arg1);
    }
  else if (name == "li")
    {
      Instruction *dest = get_reg(1);
      get_comma(2);
      // TODO: Use a correct wrapper.
      //       Sort of get_imm(3); but with correct size.
      unsigned __int128 value = get_hex_or_integer(3);
      Instruction *arg1 = current_bb->value_inst(value, BITSIZE);
      get_end_of_line(4);

      current_bb->build_inst(Op::WRITE, dest, arg1);
    }
  else if (name == "beq")
    gen_cond_branch(Op::EQ);
  else if (name == "bne")
    gen_cond_branch(Op::NE);
  else if (name == "ble")
    gen_cond_branch(Op::SLE);
  else if (name == "bleu")
    gen_cond_branch(Op::ULE);
  else if (name == "blt")
    gen_cond_branch(Op::SLT);
  else if (name == "bltu")
    gen_cond_branch(Op::ULT);
  else if (name == "bge")
    gen_cond_branch(Op::SGE);
  else if (name == "bgeu")
    gen_cond_branch(Op::UGE);
  else if (name == "bgt")
    gen_cond_branch(Op::SGT);
  else if (name == "bgtu")
    gen_cond_branch(Op::UGT);
  else if (name == "j")
    {
      Basic_block *dest_bb = get_bb(1);
      get_end_of_line(2);

      current_bb->build_br_inst(dest_bb);
      current_bb = nullptr;
    }
  else if (name == "ebreak")
    {
      current_bb->build_inst(Op::UB, current_bb->value_inst(1, 1));
      ret_bbs.push_back(current_bb);
      current_bb = nullptr;
    }
  else if (name == "ret")
    {
      ret_bbs.push_back(current_bb);
      current_bb = nullptr;
    }
  else
    throw Parse_error("unhandled instruction: "s + name, line_number);
}

void parser::lex_line(void)
{
  pos = 0;
  tokens.clear();
  while (buf[pos])
    {
      skip_space_and_comments();
      if (!buf[pos])
	break;
      if (isdigit(buf[pos]) || buf[pos] == '-')
	lex_hex_or_integer();
      else if (buf[pos] == '.' && buf[pos + 1] == 'L' ) // TODO: pos+1 check.
	lex_label_or_label_def();
      else if (isalpha(buf[pos]) || buf[pos] == '_' || buf[pos] == '.')
	lex_name();
      else if (buf[pos] == ',')
	{
	  tokens.emplace_back(lexeme::comma, pos, 1);
	  pos++;
	}
      else if (buf[pos] == '=')
	{
	  tokens.emplace_back(lexeme::assign, pos, 1);
	  pos++;
	}
      else if (buf[pos] == '[')
	{
	  tokens.emplace_back(lexeme::left_bracket, pos, 1);
	  pos++;
	}
      else if (buf[pos] == ']')
	{
	  tokens.emplace_back(lexeme::right_bracket, pos, 1);
	  pos++;
	}
      else
	throw Parse_error("Syntax error.", line_number);
    }
}

Function *parser::parse(std::string const& file_name, riscv_state *rstate)
{
  enum class state {
    global,
    function,
    basic_block,
    instruction,
    done
  };

  std::ifstream in(file_name);
  if (!in)
    throw Parse_error("Could not open file.", 0);

  modul = rstate->module;
  assert(modul->functions.size() == 1);
  src_func = modul->functions[0];

  state parser_state = state::global;
  while (parser_state != state::done && in.getline(buf, max_line_len)) {
    line_number++;

    if (parser_state == state::global)
      {
	// TODO: Implement real parsing.
	// Just eat lines until we find "foo:" for now.
	if (buf[0] == 'f' && buf[1] == 'o' && buf[2] == 'o' && buf[3] == ':')
	  {
	    current_func = modul->build_function("tgt");
	    Basic_block *entry_bb = current_func->build_bb();
	    for (int i = 0; i < 32; i++)
	      {
		Instruction *bitsize = entry_bb->value_inst(BITSIZE, 32);
		Instruction *reg = entry_bb->build_inst(Op::REGISTER, bitsize);
		registers.push_back(reg);
	      }

	    Basic_block *src_entry_bb = src_func->bbs[0];
	    for (Instruction *inst = src_entry_bb->first_inst;
		 inst;
		 inst = inst->next)
	      {
		if (inst->opcode == Op::PARAM)
		  {
		    int param_number = inst->arguments[0]->value();
		    Instruction *param_nbr =
		      current_func->bbs[0]->value_inst(param_number, 32);
		    Instruction *param_bitsize =
		      current_func->bbs[0]->value_inst(inst->bitsize, 32);
		    Instruction *param =
		      current_func->bbs[0]->build_inst(Op::PARAM, param_nbr, param_bitsize);
		    if (inst->bitsize != BITSIZE)
		      {
			Instruction *bitsize_inst = entry_bb->value_inst(BITSIZE, 32);
			if (rstate->param_is_unsigned.at(param_number))
			  param = entry_bb->build_inst(Op::ZEXT, param, bitsize_inst);
			else
			  param = entry_bb->build_inst(Op::SEXT, param, bitsize_inst);
		      }
		    entry_bb->build_inst(Op::WRITE, registers[10 + param_number], param);
		  }
	      }




	    Basic_block *bb = current_func->build_bb();
	    entry_bb->build_br_inst(bb);

	    current_bb = bb;

	    parser_state = state::function;
	  }
	continue;
      }

    lex_line();
    if (tokens.empty())
      continue;

    if (parser_state == state::function)
      {
	std::string name = get_name(&buf[tokens[0].pos]);
	if (name == ".size")
	  {
	    parser_state = state::done;
	    continue;
	  }
	parse_function();
      }
    else
      {
	throw Parse_error("Cannot happen", line_number);
      }
  }

  if (in.gcount() >= max_line_len - 1)
    throw Parse_error("line too long", line_number);
  if (parser_state != state::done)
    throw Parse_error("EOF in the middle of a function", line_number);

  // TODO: This should be a state check to ensure we are not within a function.
  // Hmm. But we probably want to check size too, but with a throw if 0.
  // Note: ret_bbs.size()); may be 0 for e.g. a function such as
  // int foo(void) {
  //     __builtin_trap();
  // }
  // Hmm. But we should treat the ebreak as a return in that case.
  Basic_block *exit_bb = current_func->build_bb();
  Basic_block *src_last_bb = src_func->bbs.back();
  assert(src_last_bb->last_inst->opcode == Op::RET);
  assert(src_last_bb->last_inst->nof_args > 0);
  uint32_t ret_size = src_last_bb->last_inst->arguments[0]->bitsize;
  Instruction *retval = exit_bb->build_inst(Op::READ, registers[10]);
  if (ret_size < retval->bitsize)
    retval = exit_bb->build_trunc(retval, ret_size);
  exit_bb->build_ret_inst(retval);
  for (auto bb : ret_bbs)
    bb->build_br_inst(exit_bb);

  return current_func;
}

} // end anonymous namespace

Function *parse_riscv(std::string const& file_name, riscv_state *state)
{
  parser p;
  Function *func = p.parse(file_name, state);
  reverse_post_order(func);
  return func;
}

} // end namespace smtgcc
