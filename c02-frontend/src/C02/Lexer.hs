-- | Low-level lexing shared by the grammar: the 'Parser' monad itself and the
-- token-level helpers (whitespace/comment skipping, symbols, keywords,
-- identifiers, literals). Nothing here depends on "C02.AST" — these are the
-- primitives the parser is built from.
module C02.Lexer
  ( Parser
  , sc
  , lexeme
  , symbol
  , keyword
  , identifier
  , intLiteralParser
  , operator
  ) where

import Text.Megaparsec
import Text.Megaparsec.Char
import qualified Text.Megaparsec.Char.Lexer as L
import Data.Void (Void)
import Data.Set (Set)
import Control.Monad.Reader (ReaderT)

-- The parser carries a read-only environment: the set of struct type names from
-- the whole-file prescan (SPEC 6.6). Reader (not State) because it's fixed before
-- parsing begins and never changes — nothing to lose on backtrack.
type Parser = ReaderT (Set String) (Parsec Void String)

-- skip whitespace/comments between tokens
sc :: Parser ()
sc = L.space space1 (L.skipLineComment "//") (L.skipBlockComment "/*" "*/")

-- wraps a parser to consume trailing whitespace automatically
lexeme :: Parser a -> Parser a
lexeme = L.lexeme sc

-- expect a symbol and clear following white space
symbol :: String -> Parser String
symbol = L.symbol sc

keyword :: String -> Parser String
keyword keyw = lexeme (string keyw <* notFollowedBy alphaNumChar)

-- Parse an identifier
identifier :: Parser String
identifier = (:) <$> (letterChar <|> char '_') <*> many (alphaNumChar <|> char '_') -- collect all chars after an alpha char

-- Parse an int literal in the various accepted formats
intLiteralParser :: Parser Int
intLiteralParser = lexeme $ choice
  [ symbol "0x" *> L.hexadecimal
  , symbol "0X" *> L.hexadecimal
  , symbol "0b" *> L.binary
  , symbol "0B" *> L.binary
  , L.decimal
  ] <* notFollowedBy alphaNumChar

-- Match an operator token, enforcing maximal munch: the same trick 'keyword'
-- uses, but guarding against operator chars instead of identifier chars. This is
-- what stops operator "<" from matching the '<' in "<<" or "<=". The inner 'try'
-- keeps a partial match (e.g. "<" of an expected "<=") non-consuming so
-- makeExprParser can fall through to another operator.
operator :: String -> Parser String
operator name = lexeme (try (string name <* notFollowedBy (oneOf opChars)))
  where opChars = "+-*/%<>=!&|^~" :: [Char]
