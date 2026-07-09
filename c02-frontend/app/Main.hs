import Text.Megaparsec
import Text.Megaparsec.Char
import qualified Text.Megaparsec.Char.Lexer as L
import Data.Void

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

data VarDecl = VarDecl
  { varType  :: BaseType
  , ptr_depth :: Int
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
  , body               :: Maybe VarDecl -- Note this will change to whatever statement type gets implemented
  } deriving (Show, Eq)


keyword :: String -> Parser String
keyword keyw = lexeme (string keyw <* notFollowedBy alphaNumChar) 


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
  _    <- lexeme $ symbol ";"                        -- consume the ';', unconditionally
  return (VarDecl ty depth name val)


-- the grammar: reg TYPE NAME @ ADDR;
regDeclParser :: Parser RegDecl
regDeclParser = do
  _    <- keyword "reg"
  ty   <- baseTypeParser
  name <- lexeme identifier
  _    <- symbol "@"
  addr <- intLiteralParser
  _    <- lexeme $ symbol ";"
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
  _                    <- symbol "{}"        -- TODO: parse body
  return FuncDecl { funcName=name, funcReturnType=ty, funcReturnPtrDepth=depth, params=p, body=Nothing }


-- Parses top level items starting with the keyword 'decl'
fwdDeclParser :: Parser TopLevelDecl
fwdDeclParser = do
  _ <- keyword "decl"
  choice
    [ do (name, p, ty, depth) <- funcSigParser
         _                    <- lexeme $ symbol ";"
         return $ FwdFuncDecl FuncDecl
           { funcName = name, funcReturnType = ty, funcReturnPtrDepth = depth
           , params = p, body = Nothing
           }
    , do (ty, depth, name) <- typeWithNameParser
         _                 <- lexeme $ symbol ";"
         return $ FwdVarDecl VarDecl
           { varType = ty, ptr_depth = depth, declName = name, declInit = Nothing
           }
    ]


-- Parse a single top level item
topLevelParser :: Parser TopLevelDecl
topLevelParser = choice
  [ RegisterDecl  <$> regDeclParser
  , GlobalVarDecl <$> varDeclParser
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
