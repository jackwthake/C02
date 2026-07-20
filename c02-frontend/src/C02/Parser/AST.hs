-- | The C02 abstract syntax tree: the shared vocabulary every frontend stage
-- (parser -> semantic analysis -> lowering to TAC) speaks in. Deliberately
-- dependency-free so it sits at the bottom of the module dependency graph and
-- no stage's types can leak into it.
module C02.Parser.AST
  ( BaseType(..)
  , TopLevelDecl(..)
  , Program(..)
  , NamedType
  , BinOp(..)
  , UnOp(..)
  , AssignmentOp(..)
  , Expr(..)
  , Block
  , Stmt(..)
  , VarDecl(..)
  , RegDecl(..)
  , StructDecl(..)
  , FuncDecl(..)
  , InclStmt(..)
  , Pos(..)
  , Loc(..)
  , locPos
  , locOffset
  , unLoc
  , validInterrupt
  ) where

-- | A source position: the file a node came from plus the byte offset of its
-- first token within that file. Both are needed because include resolution
-- splices declarations from several files into one program, so an offset alone
-- can't be resolved back to a line without knowing which file's text it indexes.
-- The 'FilePath' is stamped once per file by the parser and shared by every node
-- in that file, so carrying it inline costs one reference per file, not per node.
data Pos = Pos !FilePath !Int
  deriving (Show, Eq)

-- | A value tagged with the source position of its first token. Used only at
-- statement and top-level-declaration granularity, so semantic diagnostics can
-- report @file:line:col@ (the analyzer threads the 'Pos'; the renderer turns it
-- into a line/column against that file's source). Deliberately __transparent__ in
-- both 'Show' and 'Eq' — the wrapper prints and compares as its payload alone —
-- so the parser's AST-dump goldens are unaffected by the embedded positions.
data Loc a = Loc !Pos a

locPos :: Loc a -> Pos
locPos (Loc p _) = p

locOffset :: Loc a -> Int
locOffset (Loc (Pos _ off) _) = off

unLoc :: Loc a -> a
unLoc (Loc _ a) = a

-- showsPrec passes the precedence through so nested payloads parenthesize
-- exactly as they would unwrapped (a plain @show a@ would drop needed parens).
instance Show a => Show (Loc a) where
  showsPrec d (Loc _ a) = showsPrec d a

instance Eq a => Eq (Loc a) where
  Loc _ a == Loc _ b = a == b

data BaseType = U8 | I8 | U16 | I16 | Void
              | StructName String          -- a named struct type (SPEC §3, matched by name)
              deriving (Show, Eq)


data TopLevelDecl = GlobalVarDecl VarDecl
                  | RegisterDecl  RegDecl
                  | FunctionDecl  FuncDecl
                  | FwdFuncDecl   FuncDecl
                  | FwdVarDecl    VarDecl
                  | StructDef     StructDecl
                  deriving (Show, Eq)

data Program = Program [Loc InclStmt] [Loc TopLevelDecl] deriving Show

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
          | AddressOf                     -- &
          deriving (Show, Eq)

-- Assignment Operators
data AssignmentOp = Equals     | PlusEquals | MinusEquals   -- =   +=  -=
                  | MultEquals | DivEquals  | ModEquals     -- *=  /=  %=
                  deriving (Show, Eq)

-- An expression.
data Expr = IntLit     Int                     -- a NUMBER literal
          | StrLit     String                  -- a STRING literal
          | Var        String                  -- an IDENT reference
          | Binary     BinOp    Expr Expr      -- left <op> right
          | Unary      UnOp     Expr           -- <op> operand
          | Call       String   [Expr]         -- IDENT params
          | Cast       BaseType Int  Expr      -- <type> ptrDepth operand
          | Deref      Expr                    -- operand
          | Field      Expr     String         -- base '.' fieldname  (postfix, chains left)
          | StructInit String   [(String, Expr)] -- StructName '{' .field = expr, ... '}'
          deriving (Show, Eq)

type Block = [Loc Stmt]
data Stmt = LocVarDecl  VarDecl                                                -- Local variable
          | Nested      Block                                                  -- block of statements
          | Assign      Expr AssignmentOp Expr                                 -- left <op> right
          | Return      (Maybe Expr)                                           -- return expr?
          | If          [(Maybe Expr, Block)]                                  -- <(if-else cond?, block)> last item is list is always else
          | While       Expr (Maybe Block)                                     -- <cond> <block>?
          | For         (Maybe Stmt) (Maybe Expr) (Maybe Stmt) (Maybe Block)   -- for (<init>?; <cond>?; <incr>?) <block>?
          | ExprStmt    Expr
          | StructDeclStmt StructDecl                                          -- struct decl in statement position (SPEC §5)
          | Break                                                              -- break ;
          | Continue                                                           -- continue ;
          deriving (Show, Eq)

data InclStmt = InclStmt
  { includePath :: String } deriving (Show, Eq)

data VarDecl = VarDecl
  { varType  :: BaseType
  , ptrDepth :: Int
  , declName  :: String
  , declInit  :: Maybe Expr
  } deriving (Show, Eq)

data RegDecl = RegDecl
  { regType     :: BaseType
  , regPtrDepth :: Int
  , regName     :: String
  , declAddress :: Int
  } deriving (Show, Eq)

data StructDecl = StructDecl
  { structName   :: String
  , structFields :: [NamedType]    -- each field: (BaseType, ptrDepth, name)
  } deriving (Show, Eq)

data FuncDecl = FuncDecl
  { funcName           :: String
  , funcReturnType     :: BaseType
  , funcReturnPtrDepth :: Int
  , isInterrupt        :: Bool
  , params             :: [NamedType] -- Type, ptr depth, name
  , body               :: Maybe Stmt -- Nested variant
  } deriving (Show, Eq)


validInterrupt :: FuncDecl -> Bool
validInterrupt (FuncDecl name rt rtDepth i p _) = (name == "irq" || name == "nmi") -- Must be named irq or nmi
                                               && (rt == Void && rtDepth == 0)     -- Must return bare void
                                               && i == True && p == []             -- Must be marked interrupt and have no parameters

