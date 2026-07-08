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

-- simple AST node
data Decl = Decl
  { declType  :: BaseType
  , ptr_depth :: Int
  , declName  :: String
  , declInit  :: Maybe Int
  } deriving Show


-- Match text representation of datatypes to the actual haskell version
baseTypeParser :: Parser BaseType
baseTypeParser = choice
  [ U8   <$ symbol "u8"
  , I8   <$ symbol "i8"
  , U16  <$ symbol "u16"
  , I16  <$ symbol "i16"
  , Void <$ symbol "void"
  ]


-- Parses <typename> <ptr-depth>
typeParser :: Parser (BaseType, Int)
typeParser = do
  base  <- baseTypeParser                -- parse the type
  depth <- length <$> many (symbol "*")  -- get the number of *s which denote ptr depth
  return (base, depth)


-- the grammar: TYPE *<?>NAME (= NUMBER)? ;
declParser :: Parser Decl
declParser = do                                      -- execute impairatively
  (ty, depth) <- typeParser                          -- grab the base type and pointer depth
  name <- lexeme identifier                          -- grab the identifier
  val  <- optional (symbol "=" *> lexeme L.decimal)  -- optionally consume '=' NUMBER together
  _    <- symbol ";"                                 -- consume the ';', unconditionally
  return (Decl ty depth name val)


-- Parse an identifier
identifier :: Parser String
identifier = (:) <$> letterChar <*> many alphaNumChar -- collect all chars after an alpha char


main :: IO ()
main = do
  parseTest declParser "u8 *x = 0;"
  parseTest declParser "i8 x = 50;"
  parseTest declParser "u16 **x;"
  parseTest declParser "i16 x = 0;"
  parseTest declParser "void *x;"
  parseTest declParser "u8 *x = ;" -- should error
  parseTest declParser "u8 *x = 0" -- should error
