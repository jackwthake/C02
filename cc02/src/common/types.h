#ifndef __TYPES_H__
#define __TYPES_H__

/*
 * types.h - the language's type representation (type_t)
 * ---------------------------------------------------------
 * Pulled out of parser.h because type_t is needed by multiple independent
 * headers (parser.h for node_t, symtab.h for symbol_t, errors.h for
 * type-mismatch errors) and has no dependency on any of them itself -
 * same reasoning as arena.h: shared infrastructure with multiple
 * unrelated customers belongs in its own file, not inside whichever
 * customer happened to define it first.
 */

typedef enum {
  TYPE_U8, TYPE_I8, TYPE_U16, TYPE_I16, TYPE_VOID, TYPE_STRUCT
} type_kind_t;

typedef struct {
  type_kind_t kind;
  unsigned is_ptr;
  unsigned ptr_depth;
  char *struct_name;    // only valid when kind == TYPE_STRUCT; resolved to a decl in analysis
} type_t;

#endif // __TYPES_H__