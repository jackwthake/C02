#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "tokenizer.h"
#include "parser.h"
#include "analyzer.h"
#include "ir.h"

static const char *source =
  "reg u8 PORTA @ 0x6001;\n"
  "u8 *msg = \"hello\";\n"
  "struct Point { u8 x; u8 y; }\n"
  "fn add(u8 a, u8 b) -> u8 {\n"
  "  return a + b;\n"
  "}\n"
  "fn main() -> void {\n"
  "  u8 r = add(1, 2);\n"
  "  PORTA = r;\n"
  "}\n";

int main(void) {
  unsigned num_tokens = 0;
  long len = (long)strlen(source) + 1;
  token_t *tokens = tokenize("ir_smoke", source, len, &num_tokens);
  assert(tokens);

  parser_t parser = {0};
  assert(parser_init(&parser));
  ast_t ast = parse(&parser, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer = {0};
  assert(analyzer_init(&analyzer));
  symtab_t *sym = analyze(&analyzer, ast);
  assert(sym);
  assert(analyzer.errors == 0);

  ir_gen_t gen = {0};
  assert(ir_gen_init(&gen));
  assert(ir_gen_run(&gen, ast));

  // check declarations were collected
  assert(gen.module.reg_count == 1);
  assert(gen.module.global_count == 1);
  assert(gen.module.struct_count == 1);
  assert(gen.module.cfg_count == 2);

  // check register
  assert(gen.module.regs[0].addr == 0x6001);

  // check global string initialiser
  assert(gen.module.globals[0].init_kind == IR_INIT_STR);

  // check struct layout
  assert(gen.module.structs[0].field_count == 2);
  assert(gen.module.structs[0].fields[0].offset == 0);
  assert(gen.module.structs[0].fields[1].offset == 1);
  assert(gen.module.structs[0].total_size == 2);

  // check CFGs exist with correct names
  assert(gen.module.cfgs[0].name[0] == 'a'); // "add"
  assert(gen.module.cfgs[1].name[0] == 'm'); // "main"

  // "add" has a return statement — should have instructions
  assert(gen.module.cfgs[0].entry->instr_count > 0);

  // check "add" lowered: binop (a + b) -> TAC_ADD, then return -> TAC_RETURN
  cfg_t *add_cfg = &gen.module.cfgs[0];
  assert(add_cfg->entry->instrs[0].op == TAC_ADD);
  assert(add_cfg->entry->instrs[1].op == TAC_RETURN);

  ir_gen_free(&gen);
  analyzer_free(&analyzer);
  parser_free(&parser);
  free_tokens(tokens, num_tokens);

  // --- expression lowering coverage ---
  static const char *expr_src =
    "struct Vec2 { u8 x; u8 y; }\n"
    "fn test() -> u8 {\n"
    "  Vec2 v = Vec2 { .x = 10, .y = 20 };\n"
    "  u8 a = v.x;\n"
    "  u8 *p = &a;\n"
    "  u8 b = *p;\n"
    "  u16 w = (u16)b;\n"
    "  u8 c = !a;\n"
    "  ++c;\n"
    "  return c;\n"
    "}\n"
    "fn main() -> void {\n"
    "  test();\n"
    "}\n";

  num_tokens = 0;
  len = (long)strlen(expr_src) + 1;
  tokens = tokenize("ir_smoke_expr", expr_src, len, &num_tokens);
  assert(tokens);

  parser_t parser2 = {0};
  assert(parser_init(&parser2));
  ast = parse(&parser2, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer2 = {0};
  assert(analyzer_init(&analyzer2));
  sym = analyze(&analyzer2, ast);
  assert(sym);
  assert(analyzer2.errors == 0);

  ir_gen_t gen2 = {0};
  assert(ir_gen_init(&gen2));
  assert(ir_gen_run(&gen2, ast));

  assert(gen2.module.struct_count == 1);
  assert(gen2.module.cfg_count == 2);

  // "test" should have instructions from struct init, field access,
  // addr_of, deref, cast, unary, inc, and return
  cfg_t *test_cfg = &gen2.module.cfgs[0];
  assert(test_cfg->entry->instr_count > 0);

  ir_gen_free(&gen2);
  analyzer_free(&analyzer2);
  parser_free(&parser2);
  free_tokens(tokens, num_tokens);

  // --- statement lowering coverage ---
  static const char *stmt_src =
    "reg u8 PORTA @ 0x6001;\n"
    "fn main() -> void {\n"
    "  u8 x = 5;\n"
    "  u8 y = 10;\n"
    "  PORTA = x;\n"
    "  if (x == y) {\n"
    "    y = 1;\n"
    "  } else {\n"
    "    y = 2;\n"
    "  }\n"
    "  while (x != 0) {\n"
    "    --x;\n"
    "  }\n"
    "  for (u8 i = 0; i != 3; ++i) {\n"
    "    PORTA = i;\n"
    "  }\n"
    "}\n";

  num_tokens = 0;
  len = (long)strlen(stmt_src) + 1;
  tokens = tokenize("ir_smoke_stmt", stmt_src, len, &num_tokens);
  assert(tokens);

  parser_t parser3 = {0};
  assert(parser_init(&parser3));
  ast = parse(&parser3, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer3 = {0};
  assert(analyzer_init(&analyzer3));
  sym = analyze(&analyzer3, ast);
  assert(sym);
  assert(analyzer3.errors == 0);

  ir_gen_t gen3 = {0};
  assert(ir_gen_init(&gen3));
  assert(ir_gen_run(&gen3, ast));

  assert(gen3.module.reg_count == 1);
  assert(gen3.module.cfg_count == 1);

  cfg_t *main_cfg = &gen3.module.cfgs[0];
  assert(main_cfg->entry->instr_count > 0);

  // var decls: x = 5, y = 10
  assert(main_cfg->entry->instrs[0].op == TAC_COPY);
  assert(main_cfg->entry->instrs[1].op == TAC_COPY);

  // register store: PORTA = x
  assert(main_cfg->entry->instrs[2].op == TAC_STORE);
  assert(main_cfg->entry->instrs[2].dst.int_val == 0x6001);

  // if: condition (EQ) + negate (NOT) + conditional jump
  assert(main_cfg->entry->instrs[3].op == TAC_EQ);
  assert(main_cfg->entry->instrs[4].op == TAC_NOT);
  assert(main_cfg->entry->instrs[5].op == TAC_COND_JUMP);

  // --- serialization round-trip ---
  // Write gen3 to a temp file, read it back, and verify the module
  // is identical. This catches dangling-pointer bugs because the
  // token array is freed before reading — if any string wasn't
  // properly serialized, the loaded module will have garbage.
  const char *tmp_path = "/tmp/ir_smoke_roundtrip.o";
  assert(ir_write(&gen3, tmp_path));

  // save the instruction count before freeing gen3's arena
  unsigned expected_instr_count = main_cfg->entry->instr_count;

  ir_gen_free(&gen3);
  analyzer_free(&analyzer3);
  parser_free(&parser3);
  free_tokens(tokens, num_tokens);

  ir_gen_t gen4 = {0};
  assert(ir_read(&gen4, tmp_path));

  // declarations survived the round-trip
  assert(gen4.module.reg_count == 1);
  assert(gen4.module.regs[0].addr == 0x6001);
  assert(strcmp(gen4.module.regs[0].name, "PORTA") == 0);
  assert(gen4.module.cfg_count == 1);

  // function metadata survived
  cfg_t *rt_cfg = &gen4.module.cfgs[0];
  assert(strcmp(rt_cfg->name, "main") == 0);
  assert(rt_cfg->return_type.kind == TYPE_VOID);

  // instruction stream survived
  assert(rt_cfg->entry->instr_count == expected_instr_count);
  assert(rt_cfg->entry->instrs[0].op == TAC_COPY);
  assert(rt_cfg->entry->instrs[2].op == TAC_STORE);
  assert(rt_cfg->entry->instrs[2].dst.int_val == 0x6001);
  assert(rt_cfg->entry->instrs[3].op == TAC_EQ);
  assert(rt_cfg->entry->instrs[5].op == TAC_COND_JUMP);

  // string operands survived (not dangling pointers)
  assert(rt_cfg->entry->instrs[0].dst.kind == OPERAND_VAR);
  assert(strcmp(rt_cfg->entry->instrs[0].dst.name, "x") == 0);

  ir_gen_free(&gen4);
  remove(tmp_path);

  // --- register read coverage (issue 1) ---
  static const char *regread_src =
    "reg u8 PORTA @ 0x6001;\n"
    "fn main() -> void {\n"
    "  u8 x;\n"
    "  x = PORTA;\n"
    "}\n";

  num_tokens = 0;
  len = (long)strlen(regread_src) + 1;
  tokens = tokenize("ir_smoke_regread", regread_src, len, &num_tokens);
  assert(tokens);

  parser_t parser5 = {0};
  assert(parser_init(&parser5));
  ast = parse(&parser5, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer5 = {0};
  assert(analyzer_init(&analyzer5));
  sym = analyze(&analyzer5, ast);
  assert(sym && analyzer5.errors == 0);

  ir_gen_t gen5 = {0};
  assert(ir_gen_init(&gen5));
  assert(ir_gen_run(&gen5, ast));

  cfg_t *regread_cfg = &gen5.module.cfgs[0];
  // x = PORTA → TAC_LOAD from 0x6001 into a temp, then TAC_COPY temp → x
  assert(regread_cfg->entry->instrs[0].op == TAC_LOAD);
  assert(regread_cfg->entry->instrs[0].src1.int_val == 0x6001);
  assert(regread_cfg->entry->instrs[1].op == TAC_COPY);

  ir_gen_free(&gen5);
  analyzer_free(&analyzer5);
  parser_free(&parser5);
  free_tokens(tokens, num_tokens);

  // --- short-circuit && coverage (issue 3) ---
  static const char *sc_src =
    "fn main() -> void {\n"
    "  u8 a = 0;\n"
    "  u8 b = 1;\n"
    "  u8 c = a && b;\n"
    "}\n";

  num_tokens = 0;
  len = (long)strlen(sc_src) + 1;
  tokens = tokenize("ir_smoke_sc", sc_src, len, &num_tokens);
  assert(tokens);

  parser_t parser6 = {0};
  assert(parser_init(&parser6));
  ast = parse(&parser6, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer6 = {0};
  assert(analyzer_init(&analyzer6));
  sym = analyze(&analyzer6, ast);
  assert(sym && analyzer6.errors == 0);

  ir_gen_t gen6 = {0};
  assert(ir_gen_init(&gen6));
  assert(ir_gen_run(&gen6, ast));

  cfg_t *sc_cfg = &gen6.module.cfgs[0];
  // a = 0, b = 1, then short-circuit: NOT, COND_JUMP (b eval guarded)
  assert(sc_cfg->entry->instrs[0].op == TAC_COPY); // a = 0
  assert(sc_cfg->entry->instrs[1].op == TAC_COPY); // b = 1
  // && lowering: a loaded, NOT, COND_JUMP — b is only evaluated after the jump
  int found_cond_jump = 0;
  int found_copy_after_jump = 0;
  for (unsigned k = 2; k < sc_cfg->entry->instr_count; k++) {
    if (sc_cfg->entry->instrs[k].op == TAC_COND_JUMP) found_cond_jump = 1;
    if (found_cond_jump && sc_cfg->entry->instrs[k].op == TAC_COPY) {
      found_copy_after_jump = 1;
      break;
    }
  }
  assert(found_cond_jump);
  assert(found_copy_after_jump);

  ir_gen_free(&gen6);
  analyzer_free(&analyzer6);
  parser_free(&parser6);
  free_tokens(tokens, num_tokens);

  // --- forward declaration + extern round-trip ---
  static const char *fwd_src =
    "decl fn send_byte(u8 b) -> void;\n"
    "decl u8 counter;\n"
    "fn main() -> void {\n"
    "  send_byte(42);\n"
    "  u8 x = counter;\n"
    "}\n";

  num_tokens = 0;
  len = (long)strlen(fwd_src) + 1;
  tokens = tokenize("ir_smoke_fwd", fwd_src, len, &num_tokens);
  assert(tokens);

  parser_t parser_fwd = {0};
  assert(parser_init(&parser_fwd));
  ast = parse(&parser_fwd, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer_fwd = {0};
  assert(analyzer_init(&analyzer_fwd));
  sym = analyze(&analyzer_fwd, ast);
  assert(sym && analyzer_fwd.errors == 0);

  ir_gen_t gen_fwd = {0};
  assert(ir_gen_init(&gen_fwd));
  assert(ir_gen_run(&gen_fwd, ast));

  assert(gen_fwd.module.extern_count == 2);
  assert(gen_fwd.module.extern_count == 2);
  assert(gen_fwd.module.externs[0].is_function == 1);
  assert(strcmp(gen_fwd.module.externs[0].name, "send_byte") == 0);
  assert(gen_fwd.module.externs[0].params.count == 1);
  assert(gen_fwd.module.externs[1].is_function == 0);
  assert(strcmp(gen_fwd.module.externs[1].name, "counter") == 0);

  // round-trip: write then read back
  const char *fwd_tmp = "/tmp/ir_smoke_fwd_roundtrip.o";
  assert(ir_write(&gen_fwd, fwd_tmp));

  ir_gen_free(&gen_fwd);
  analyzer_free(&analyzer_fwd);
  parser_free(&parser_fwd);
  free_tokens(tokens, num_tokens);

  ir_gen_t gen_fwd_rt = {0};
  assert(ir_read(&gen_fwd_rt, fwd_tmp));

  assert(gen_fwd_rt.module.extern_count == 2);
  assert(gen_fwd_rt.module.externs[0].is_function == 1);
  assert(strcmp(gen_fwd_rt.module.externs[0].name, "send_byte") == 0);
  assert(gen_fwd_rt.module.externs[0].params.count == 1);
  assert(strcmp(gen_fwd_rt.module.externs[0].params.items[0].name, "b") == 0);
  assert(gen_fwd_rt.module.externs[0].type.kind == TYPE_VOID);
  assert(gen_fwd_rt.module.externs[1].is_function == 0);
  assert(strcmp(gen_fwd_rt.module.externs[1].name, "counter") == 0);
  assert(gen_fwd_rt.module.externs[1].type.kind == TYPE_U8);

  ir_gen_free(&gen_fwd_rt);
  remove(fwd_tmp);

  // --- number literal resolved_type coverage (issue 4) ---
  static const char *neglit_src =
    "fn main() -> void {\n"
    "  i8 x = -5;\n"
    "}\n";

  num_tokens = 0;
  len = (long)strlen(neglit_src) + 1;
  tokens = tokenize("ir_smoke_neglit", neglit_src, len, &num_tokens);
  assert(tokens);

  parser_t parser7 = {0};
  assert(parser_init(&parser7));
  ast = parse(&parser7, tokens, num_tokens);
  assert(ast);

  analyzer_t analyzer7 = {0};
  assert(analyzer_init(&analyzer7));
  sym = analyze(&analyzer7, ast);
  assert(sym && analyzer7.errors == 0);

  ir_gen_t gen7 = {0};
  assert(ir_gen_init(&gen7));
  assert(ir_gen_run(&gen7, ast));

  cfg_t *neglit_cfg = &gen7.module.cfgs[0];
  // i8 x = -5 → TAC_NEG on a literal 5 typed i8, then TAC_COPY
  assert(neglit_cfg->entry->instrs[0].op == TAC_NEG);
  assert(neglit_cfg->entry->instrs[0].src1.type.kind == TYPE_I8);
  assert(neglit_cfg->entry->instrs[1].op == TAC_COPY);

  ir_gen_free(&gen7);
  analyzer_free(&analyzer7);
  parser_free(&parser7);
  free_tokens(tokens, num_tokens);

  printf("all ir smoke tests passed\n");
  return 0;
}
