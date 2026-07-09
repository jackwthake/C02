-- | The C02 abstract syntax tree: the shared vocabulary every frontend stage
-- (parser -> semantic analysis -> lowering to TAC) speaks in. Deliberately
-- dependency-free so it sits at the bottom of the module dependency graph and
-- no stage's types can leak into it.
module C02.AST
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
  ) where

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

type Block = [Stmt]
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


data VarDecl = VarDecl
  { varType  :: BaseType
  , ptrDepth :: Int
  , declName  :: String
  , declInit  :: Maybe Expr
  } deriving (Show, Eq)

data RegDecl = RegDecl
  { regType     :: BaseType
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
