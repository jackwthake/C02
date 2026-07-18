{-# OPTIONS_GHC -Wno-unused-top-binds #-}

-- | The C02 grammar: turns source text into a "C02.AST" 'Program'. Built on the
-- token helpers in "C02.Lexer" and megaparsec's expression-combinator support.
-- 'parseProgram' is the stage entry point the driver (and later stages) call.
module C02.Parser.Parser
  ( parseProgram
  , programParser
  , prescanStructNames
  ) where

import Text.Megaparsec
import Control.Monad.Combinators.Expr
import Control.Monad (void, when)
import Data.Maybe (isNothing, maybeToList, isJust)
import Data.Void (Void)
import Data.Set (Set)
import qualified Data.Set as Set
import Control.Monad.Reader (runReaderT, asks)

import C02.Parser.AST
import C02.Parser.Lexer

-- Tag a parse with the source byte offset of its first token. Because lexemes
-- consume trailing whitespace/comments, the offset captured here (before the
-- inner parser runs) sits on the next real token, not on leading layout — so a
-- statement/decl's position lands on its first token. Used only at statement and
-- top-level granularity (see 'Loc').
located :: Parser a -> Parser (Loc a)
located p = Loc <$> getOffset <*> p


-- Left-factored: parse the identifier once, then decide on what follows.
--   '(' -> Call,  '{' -> struct init,  otherwise a plain Var reference.
-- Each branch's first token is a distinct symbol, so 'choice' picks without
-- backtracking (symbol fails without consuming when the char doesn't match).
identExprParser :: Parser Expr
identExprParser = do
  name <- lexeme identifier
  choice
    [ Call       name <$> (symbol "(" *> sepEndBy exprParser (symbol ",") <* symbol ")")
    , StructInit name <$> (symbol "{" *> sepEndBy fieldInit  (symbol ",") <* symbol "}")
    , pure (Var name)
    ]


-- one '.field = expr' entry inside a struct initializer (SPEC init_list)
fieldInit :: Parser (String, Expr)
fieldInit = do
  _   <- symbol "."
  fld <- lexeme identifier
  _   <- symbol "="
  val <- exprParser
  return (fld, val)


-- (type) unary : the cast operand binds at 'unary' precedence (SPEC 6.6), so
-- `(u8)w / 2` is `((u8)w) / 2`, not `(u8)(w / 2)`. 'unaryParser' also covers a
-- nested cast (via primary), so `(u8)(u16)x` stacks right; wrap the operand in
-- explicit parens to cast a whole binary expression.
castParser :: Parser Expr
castParser = do
  _           <- symbol "("
  (ty, depth) <- typeParser
  _           <- symbol ")"
  operand     <- unaryParser
  return (Cast ty depth operand)


groupParser :: Parser Expr
groupParser = do
  _       <- symbol "("
  operand <- exprParser
  _       <- symbol ")"
  return operand


primaryParser :: Parser Expr
primaryParser = choice
  [ IntLit <$> intLiteralParser
  , IntLit <$> charLiteralParser
  , StrLit <$> stringLiteralParser
  , try castParser <|> groupParser     -- both open with '('
  , identExprParser                    -- identifier: Call / struct-init / Var
  ]


-- Postfix field access: a primary followed by zero or more '.field' (SPEC 6.4).
-- Left-associative, so 'a.b.c' builds (a.b).c via foldl. This sits between the
-- prefix-unary chain and primary, so '.' binds tighter than any prefix operator.
postfixParser :: Parser Expr
postfixParser = do
  base <- primaryParser
  flds <- many (symbol "." *> lexeme identifier)
  return (foldl Field base flds)


-- The prefix-unary chain: right-associative and self-recursive, so operators
-- stack (--*p, &*p, !!x). '*' and '@' are interchangeable spellings of the same
-- dereference (SPEC 6), so both map to Deref. This is the real base term fed to
-- makeExprParser -- when no prefix operator is present it falls through to a primary.
unaryParser :: Parser Expr
unaryParser = choice
  [ Unary Incr      <$> (symbol "++" *> unaryParser)   -- '++'/'--' listed before '-'
  , Unary Decr      <$> (symbol "--" *> unaryParser)   -- so maximal munch picks them
  , Unary Bang      <$> (symbol "!"  *> unaryParser)
  , Unary BitNot    <$> (symbol "~"  *> unaryParser)
  , Unary Negate    <$> (symbol "-"  *> unaryParser)
  , Unary AddressOf <$> (symbol "&"  *> unaryParser)
  , Deref           <$> (symbol "*"  *> unaryParser)
  , Deref           <$> (symbol "@"  *> unaryParser)
  , postfixParser                                      -- postfix (field access) then primary
  ]


-- Build a left-associative binary operator for the precedence table:
-- parse the operator token, and yield the constructor that combines both operands.
binL :: String -> BinOp -> Operator Parser Expr
binL name op = InfixL (Binary op <$ operator name)


-- The precedence ladder, tightest-binding level FIRST (loosest last).
operatorTable :: [[Operator Parser Expr]]
operatorTable =
  [ [ binL "*" Mul, binL "/" Div, binL "%" Mod ]         -- factor
  , [ binL "+" Add, binL "-" Sub ]                       -- term
  , [ binL "<<" Shl, binL ">>" Shr ]                     -- shift
  , [ binL "<=" Le, binL ">=" Ge, binL "<" Lt, binL ">" Gt ]  -- comparison
  , [ binL "==" Eq, binL "!=" Ne ]                       -- equality
  , [ binL "&" BitAnd ]                                  -- bitwise and
  , [ binL "^" BitXor ]                                  -- bitwise xor
  , [ binL "|" BitOr ]                                   -- bitwise or
  , [ binL "&&" And ]                                    -- logical and
  , [ binL "||" Or ]                                     -- logical or
  ]


-- The full expression parser: unaryParser as the base term, with the binary
-- operator table layered on top by makeExprParser.
exprParser :: Parser Expr
exprParser = makeExprParser unaryParser operatorTable


-- parse a block
braceBlock :: Parser Block                       -- just the list
braceBlock = symbol "{" *> many (located statementParser) <* symbol "}"

blockParser :: Parser Stmt                       -- the statement form
blockParser = Nested <$> braceBlock


-- parse an assignmnet operator
assignmnetOperatorParser :: Parser AssignmentOp
assignmnetOperatorParser = choice
  [ Equals      <$ symbol "="
  , PlusEquals  <$ symbol "+="
  , MinusEquals <$ symbol "-="
  , MultEquals  <$ symbol "*="
  , DivEquals   <$ symbol "/="
  , ModEquals   <$ symbol "%="
  ]


-- parse assignmnet statement or call site - they have ambiguous starts, both with an expression
exprOrAssignParser :: Parser Stmt
exprOrAssignParser = do
  left <- exprParser                         -- both assignmnets and calls start with an expression
  choice
    [ do op    <- assignmnetOperatorParser   -- attempt to parse as assignmnet
         right <- exprParser
         return (Assign left op right)
    , pure (ExprStmt left)                   -- if assignmnet parsing fails, parse as expression statement
    ]


-- parse a return statement
returnParser :: Parser Stmt
returnParser = do
  _    <- keyword "return"
  expr <- optional exprParser
  _    <- symbol ";"
  return (Return expr)


-- ( expr )
parenExpr :: Parser Expr
parenExpr = symbol "(" *> exprParser <* symbol ")"


-- ( expr ) { block }  -- the part 'if' and 'else if' share
condBlock :: Parser (Expr, Block)
condBlock = (,) <$> parenExpr <*> braceBlock


elseIf :: Parser (Maybe Expr, Block)
elseIf = do
  _        <- try (keyword "else" *> keyword "if")   -- see note on 'try'
  (c, blk) <- condBlock
  return (Just c, blk)


elseBlock :: Parser (Maybe Expr, Block)
elseBlock = do
  _   <- keyword "else"
  blk <- braceBlock
  return (Nothing, blk)                             -- Nothing = the 'else' clause


ifParser :: Parser Stmt
ifParser = do
  _         <- keyword "if"
  (c, blk)  <- condBlock
  elifs     <- many elseIf          -- :: [(Maybe Expr, Block)]  (0 or more)
  els       <- optional elseBlock   -- :: Maybe (Maybe Expr, Block)  (0 or 1)
  return (If ((Just c, blk) : elifs ++ maybeToList els))


whileParser :: Parser Stmt
whileParser = do
  _ <- keyword "while"
  cond <- parenExpr
  blk <- optional braceBlock               -- while bodies not required - while(true); is valid
  when (isNothing blk) $ void (symbol ";") -- require ; after while if theres no body
  return (While cond blk)


-- a single for-header clause: a statement WITHOUT its terminating ';'.
-- the 'for' grammar owns the ';' separators, so the clause must not eat one.
forClause :: Parser Stmt
forClause = LocVarDecl <$> varDeclParser   -- e.g.  u8 i = 0
        <|> exprOrAssignParser             -- e.g.  i = 0   or   ++i


forParser :: Parser Stmt
forParser = do
  _ <- keyword "for"
  _ <- symbol "("
  initr <- optional forClause
  _ <- symbol ";"
  cond <- optional exprParser
  _ <- symbol ";"
  incr <- optional forClause
  _ <- symbol ")"
  blk <- optional braceBlock               -- for bodies not required - for(;;); is valid
  when (isNothing blk) $ void (symbol ";") -- require ; after for if theres no body
  return (For initr cond incr blk)


-- parse a single statement
statementParser :: Parser Stmt
statementParser = choice
  [ StructDeclStmt <$> structDeclParser
  , LocVarDecl <$> varDeclParser <* symbol ";"
  , blockParser
  , returnParser
  , Break    <$ (keyword "break"    *> symbol ";")
  , Continue <$ (keyword "continue" *> symbol ";")
  , ifParser
  , whileParser
  , forParser
  , exprOrAssignParser <* symbol ";"
  ]


-- Match text representation of datatypes to the actual haskell version
baseTypeParser :: Parser BaseType
baseTypeParser = choice   -- try each variant in order
  [ U8   <$ keyword "u8"
  , I8   <$ keyword "i8"
  , U16  <$ keyword "u16"
  , I16  <$ keyword "i16"
  , Void <$ keyword "void"
  , structTypeParser        -- an identifier naming a prescanned struct
  ]


-- An identifier is a struct type iff it's in the prescanned name set (SPEC 6.6).
-- 'try' so a non-struct identifier is left unconsumed, letting the caller fall
-- through (e.g. `p.x = 5;` reads as an expr-statement, not a failed declaration).
structTypeParser :: Parser BaseType
structTypeParser = try $ do
  name  <- lexeme identifier
  known <- asks (Set.member name)
  if known then return (StructName name)
           else fail ("`" ++ name ++ "` is not a struct type")


-- Parses <typename> <ptr-depth>
typeParser :: Parser (BaseType, Int)
typeParser = do
  base  <- baseTypeParser                -- parse the type
  depth <- length <$> many (symbol "*")  -- get the number of *s which denote ptr depth
  return (base, depth)


-- Parse a single type with name: <TYPE> *<?>NAME
-- Used for params and vardecls / fwd var decls
typeWithNameParser :: Parser NamedType
typeWithNameParser = do
  (ty, depth) <- typeParser
  name <- lexeme identifier
  return (ty, depth, name )


-- the grammar: TYPE *<?>NAME (= EXPR)? ;
-- 'try' spans the whole type-then-name head: a struct name followed by
-- something other than a name (e.g. the statement `Point{ .x = 1 };`) must
-- backtrack fully so the caller can re-read it as an expression. Once the
-- name is in hand we're committed — a bad initializer is a hard error.
varDeclParser :: Parser VarDecl
varDeclParser = do
  (ty, depth, name) <- try typeWithNameParser        -- grab the base type, pointer depth, and name
  val  <- optional (symbol "=" *> exprParser)        -- optionally consume '=' EXPR together
  return (VarDecl ty depth name val)


-- the grammar: reg TYPE NAME @ ADDR;
-- The type takes pointer stars like any other (SPEC 4.3: `reg u8 *X @ ...;`
-- parses; the semantics are the analyzer's problem, not ours).
regDeclParser :: Parser RegDecl
regDeclParser = do
  _           <- keyword "reg"
  (ty, depth) <- typeParser
  name        <- lexeme identifier
  _           <- symbol "@"
  addr        <- intLiteralParser
  _           <- symbol ";"
  return (RegDecl ty depth name addr)


-- struct_decl ::= "struct" IDENT "{" field_decl* "}" ";"?   (SPEC §2/§4)
-- The name was already collected by the prescan; here we parse the real shape.
structDeclParser :: Parser StructDecl
structDeclParser = do
  _      <- keyword "struct"
  name   <- lexeme identifier
  _      <- symbol "{"
  fields <- many fieldDeclParser
  _      <- symbol "}"
  _      <- optional (symbol ";")               -- trailing ';' is optional
  return (StructDecl name fields)


-- field_decl ::= type IDENT ";"
fieldDeclParser :: Parser NamedType
fieldDeclParser = typeWithNameParser <* symbol ";"


-- Parses 'fn NAME(PARAMS) -> RETURN_TYPE
--                       Name    Params                     Ret Base  Ret ptr depth
funcSigParser :: Parser (String, [NamedType], BaseType, Int, Bool)
funcSigParser = do
  _            <- keyword "fn"
  name         <- lexeme identifier
  _            <- symbol "("
  p            <- sepEndBy typeWithNameParser (symbol ",")
  _            <- symbol ")"
  mInterrupt   <- optional (keyword "interrupt")
  let isInt     = isJust mInterrupt
  _            <- symbol "->"
  (ty, depth)  <- typeParser
  return (name, p, ty, depth, isInt)


-- parse function with body
funcDeclParser :: Parser FuncDecl
funcDeclParser = do
  (name, p, ty, depth, isInt) <- funcSigParser
  blk <- blockParser
  return FuncDecl { funcName=name, funcReturnType=ty, funcReturnPtrDepth=depth, isInterrupt = isInt, params=p, body= Just blk }


-- Parses top level items starting with the keyword 'decl'
fwdDeclParser :: Parser TopLevelDecl
fwdDeclParser = do
  _ <- keyword "decl"
  choice
    [ do (name, p, ty, depth, isInt) <- funcSigParser
         _                    <- symbol ";"
         return $ FwdFuncDecl FuncDecl
           { funcName = name, funcReturnType = ty, funcReturnPtrDepth = depth
           , isInterrupt = isInt, params = p, body = Nothing
           }
    , do (ty, depth, name) <- typeWithNameParser
         _                 <- symbol ";"
         return $ FwdVarDecl VarDecl
           { varType = ty, ptrDepth = depth, declName = name, declInit = Nothing
           }
    ]

includeParser :: Parser InclStmt
includeParser = do
  _    <- keyword "include"
  path <- stringLiteralParser
  _    <- symbol ";"
  return (InclStmt path)

-- Parse a single top level item
topLevelParser :: Parser TopLevelDecl
topLevelParser = choice
  [ IncludeStmt   <$> includeParser 
  , StructDef     <$> structDeclParser
  , RegisterDecl  <$> regDeclParser
  , GlobalVarDecl <$> varDeclParser <* symbol ";"
  , FunctionDecl  <$> funcDeclParser
  , fwdDeclParser
  ]


-- Parse a program
programParser :: Parser Program
programParser = do
  sc                            -- eat any leading whitespace/comments before the first token
  decls <- many (located topLevelParser)  -- each top-level decl tagged with its offset
  eof                           -- fail if anything is left unconsumed
  return (TopLevels decls)


-- Whole-file, scope-blind prescan for struct type names (SPEC 6.6). Walks the
-- token stream reusing 'sc', so a 'struct' inside a comment or string literal is
-- ignored, and collects the NAME from every 'struct IDENT {'. Runs once before
-- the real parse (structs are forward-reference-tolerant, so we need them all up
-- front). Best-effort: on any parse hiccup we return what we have.
prescanStructNames :: String -> Set String
prescanStructNames src =
  -- run with an empty env: the prescan never consults the set it helps build
  Set.fromList (either (const []) id (parse (runReaderT (sc *> go) Set.empty) "" src))
  where
    go :: Parser [String]
    go = choice
      [ [] <$ eof
      , (:) <$> try structHead <*> go     -- 'struct NAME {'  ->  record NAME
      , anyToken *> go                     -- anything else    ->  skip one token, continue
      ]
    -- the pattern we care about; 'try' so a bare 'struct' that isn't a decl rolls back
    structHead :: Parser String
    structHead = keyword "struct" *> lexeme identifier <* symbol "{"
    -- consume exactly one token then trailing layout. Strings and identifiers are
    -- taken WHOLE so we never char-slide into a substring like the 'struct' in
    -- 'astruct'; 'sc' at the end skips whitespace + comments to the next boundary.
    anyToken :: Parser ()
    anyToken = (void stringLiteralParser <|> void identifier <|> void anySingle) *> sc


-- | Parse a whole source file into a 'Program'. Runs the struct-name prescan
-- (SPEC 6.6) and threads the resulting set through the parser as its environment.
parseProgram :: FilePath -> String -> Either (ParseErrorBundle String Void) Program
parseProgram path src =
  parse (runReaderT programParser (prescanStructNames src)) path src
