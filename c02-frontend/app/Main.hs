{-# OPTIONS_GHC -Wno-unused-top-binds #-}
import Text.Megaparsec
import Text.Megaparsec.Char
import qualified Text.Megaparsec.Char.Lexer as L
import Control.Monad.Combinators.Expr
import Control.Monad
import Data.Void
import Data.Maybe (isNothing, maybeToList)

type Parser = Parsec Void String

-- skip whitespace/comments between tokens
sc :: Parser ()
sc = L.space space1 (L.skipLineComment "//") (L.skipBlockComment "/*" "*/")

-- wraps a parser to consume trailing whitespace automatically
lexeme :: Parser a -> Parser a
lexeme = L.lexeme sc

-- expect a symbol and clear following white space
symbol :: String -> Parser String
symbol = L.symbol sc


data BaseType = U8 | I8 | U16 | I16 | Void deriving (Show, Eq)


data TopLevelDecl = GlobalVarDecl VarDecl
                  | RegisterDecl  RegDecl
                  | FunctionDecl  FuncDecl
                  | FwdFuncDecl   FuncDecl
                  | FwdVarDecl    VarDecl
                  deriving (Show, Eq)

data Program = TopLevels [ TopLevelDecl ] deriving Show

--                Type      Ptr Depth   Name
type NamedType = (BaseType, Int,        String)


-- Binary operators
data BinOp = Add | Sub | Mul | Div | Mod   -- +   -   *   /   %
           | Shl | Shr                     -- <<  >>
           | Lt  | Gt  | Le  | Ge          -- <   >   <=  >=
           | Eq  | Ne                      -- ==  !=
           | BitAnd | BitXor | BitOr       -- &   ^   |
           | And | Or                      -- &&  ||
           deriving (Show, Eq)

-- Unary Operators
data UnOp = Incr      | Decr              -- ++   -- 
          | Bang      | Negate | BitNot   -- !    -   ~
          | AddressOf                     -- &/@
          deriving (Show, Eq)

-- Assignment Operators
data AssignmentOp = Equals     | PlusEquals | MinusEquals   -- =   +=  -=
                  | MultEquals | DivEquals  | ModEquals     -- *=  /=  %=
                  deriving (Show, Eq)

-- An expression.
-- TODO: field access, struct init.
data Expr = IntLit     Int                   -- a NUMBER literal
          | StrLit     String                -- a STRING literal
          | Var        String                -- an IDENT reference
          | Binary     BinOp    Expr Expr    -- left <op> right
          | Unary      UnOp     Expr         -- <op> operand 
          | Call       String   [Expr]       -- IDENT params
          | Cast       BaseType Int  Expr    -- <type> ptrDepth operand
          | Deref      Expr                  -- operand
          deriving (Show, Eq)

type Block = [Stmt] 
data Stmt = LocVarDecl  VarDecl                                                -- Local variable
          | Nested      Block                                                  -- block of statements                                         
          | Assign      Expr AssignmentOp Expr                                 -- left <op> right
          | Return      (Maybe Expr)                                           -- return expr?
          | If          [(Maybe Expr, Block)]                                  -- <(if-else cond?, block)> last item is list is always else
          | While       Expr (Maybe Block)                                     -- <cond> <block>?
          | For         (Maybe Stmt) (Maybe Expr) (Maybe Stmt) (Maybe Block)   -- for (<init>?; <cond>?; <incr>?) <block>? 
          | ExprStmt    Expr
          deriving (Show, Eq)


data VarDecl = VarDecl
  { varType  :: BaseType
  , ptrDepth :: Int
  , declName  :: String
  , declInit  :: Maybe Int
  } deriving (Show, Eq)

data RegDecl = RegDecl
  { regType     :: BaseType
  , regName     :: String
  , declAddress :: Int
  } deriving (Show, Eq)

data FuncDecl = FuncDecl
  { funcName           :: String
  , funcReturnType     :: BaseType
  , funcReturnPtrDepth :: Int
  , params             :: [NamedType] -- Type, ptr depth, name
  , body               :: Maybe Stmt -- Nested variant
  } deriving (Show, Eq)


keyword :: String -> Parser String
keyword keyw = lexeme (string keyw <* notFollowedBy alphaNumChar) 


callParser :: Parser Expr
callParser = do
  name <- lexeme identifier
  _    <- symbol "("
  args <- sepBy exprParser $ symbol ","
  _    <- symbol ")"
  return (Call name args)


varParser :: Parser Expr
varParser = Var <$> lexeme identifier


-- (type) expr : the cast operand is a full expression, per SPEC 6.
castParser :: Parser Expr
castParser = do
  _           <- symbol "("
  (ty, depth) <- typeParser
  _           <- symbol ")"
  operand     <- exprParser
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
  , StrLit <$> lexeme (char '"' *> manyTill L.charLiteral (char '"'))
  , try castParser <|> groupParser     -- both open with '('
  , try callParser <|> varParser       -- both open with an identifier
  ]


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
  , primaryParser
  ]


-- Match an operator token, enforcing maximal munch: the same trick 'keyword'
-- uses, but guarding against operator chars instead of identifier chars. This is
-- what stops operator "<" from matching the '<' in "<<" or "<=". The inner 'try'
-- keeps a partial match (e.g. "<" of an expected "<=") non-consuming so
-- makeExprParser can fall through to another operator.
operator :: String -> Parser String
operator name = lexeme (try (string name <* notFollowedBy (oneOf opChars)))
  where opChars = "+-*/%<>=!&|^~" :: [Char]


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
braceBlock = symbol "{" *> many statementParser <* symbol "}"

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
  [ LocVarDecl <$> varDeclParser <* symbol ";"
  , blockParser
  , returnParser
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
  ]


-- Parses <typename> <ptr-depth>
typeParser :: Parser (BaseType, Int)
typeParser = do
  base  <- baseTypeParser                -- parse the type
  depth <- length <$> many (symbol "*")  -- get the number of *s which denote ptr depth
  return (base, depth)


-- Parse an int literal in the various accepted formats
intLiteralParser :: Parser Int
intLiteralParser = lexeme $ choice
  [ symbol "0x" *> L.hexadecimal
  , symbol "0X" *> L.hexadecimal
  , symbol "0b" *> L.binary
  , symbol "0B" *> L.binary
  , L.decimal
  ] <* notFollowedBy alphaNumChar


-- Parse an identifier
identifier :: Parser String
identifier = (:) <$> (letterChar <|> char '_') <*> many (alphaNumChar <|> char '_') -- collect all chars after an alpha char


-- Parse a single type with name: <TYPE> *<?>NAME
-- Used for params and vardecls / fwd var decls
typeWithNameParser :: Parser NamedType
typeWithNameParser = do
  (ty, depth) <- typeParser
  name <- lexeme identifier 
  return (ty, depth, name )


-- the grammar: TYPE *<?>NAME (= NUMBER)? ;
varDeclParser :: Parser VarDecl
varDeclParser = do                                   -- execute impairatively
  (ty, depth, name) <- typeWithNameParser            -- grab the base type, pointer depth, and name                          -- grab the identifier
  val  <- optional (symbol "=" *> intLiteralParser)  -- optionally consume '=' NUMBER together
  return (VarDecl ty depth name val)


-- the grammar: reg TYPE NAME @ ADDR;
regDeclParser :: Parser RegDecl
regDeclParser = do
  _    <- keyword "reg"
  ty   <- baseTypeParser
  name <- lexeme identifier
  _    <- symbol "@"
  addr <- intLiteralParser
  _    <- symbol ";"
  return (RegDecl ty name addr)


-- Parses 'fn NAME(PARAMS) -> RETURN_TYPE
--                       Name    Params                     Ret Base  Ret ptr depth
funcSigParser :: Parser (String, [NamedType], BaseType, Int)
funcSigParser = do
  _           <- keyword "fn"
  name        <- lexeme identifier
  _           <- symbol "("
  p           <- sepBy typeWithNameParser (symbol ",")
  _           <- symbol ")"
  _           <- symbol "->"
  (ty, depth) <- typeParser
  return (name, p, ty, depth)


-- parse function with body
-- NOTE: so far this just parses functions an empty block
funcDeclParser :: Parser FuncDecl
funcDeclParser = do
  (name, p, ty, depth) <- funcSigParser
  blk <- blockParser
  return FuncDecl { funcName=name, funcReturnType=ty, funcReturnPtrDepth=depth, params=p, body= Just blk }


-- Parses top level items starting with the keyword 'decl'
fwdDeclParser :: Parser TopLevelDecl
fwdDeclParser = do
  _ <- keyword "decl"
  choice
    [ do (name, p, ty, depth) <- funcSigParser
         _                    <- symbol ";"
         return $ FwdFuncDecl FuncDecl
           { funcName = name, funcReturnType = ty, funcReturnPtrDepth = depth
           , params = p, body = Nothing
           }
    , do (ty, depth, name) <- typeWithNameParser
         _                 <- symbol ";"
         return $ FwdVarDecl VarDecl
           { varType = ty, ptrDepth = depth, declName = name, declInit = Nothing
           }
    ]


-- Parse a single top level item
topLevelParser :: Parser TopLevelDecl
topLevelParser = choice
  [ RegisterDecl  <$> regDeclParser
  , GlobalVarDecl <$> varDeclParser <* symbol ";"
  , FunctionDecl  <$> funcDeclParser
  , fwdDeclParser
  ]


-- Parse a program
programParser :: Parser Program
programParser = do
  sc                            -- eat any leading whitespace/comments before the first token
  decls <- many topLevelParser  -- iterate through the available tokens, parsing top level items
  eof                           -- fail if anything is left unconsumed
  return (TopLevels decls)


parseFile :: FilePath -> IO ()
parseFile fileName = do
  input <- readFile fileName                        -- Read file
  case parse programParser fileName input of        -- Run parser catching either success or print the error message
    Left e        -> putStr $ errorBundlePretty e
    Right program -> print program


main :: IO ()
main = parseFile "./test.c02"
