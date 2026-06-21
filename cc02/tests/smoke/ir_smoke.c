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
  assert(ir_gen_run(&gen, ast, &analyzer));

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
  assert(ir_gen_run(&gen2, ast, &analyzer2));

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

  printf("all ir smoke tests passed\n");
  return 0;
}
