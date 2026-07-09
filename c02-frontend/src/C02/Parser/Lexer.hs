-- | Low-level lexing shared by the grammar: the 'Parser' monad itself and the
-- token-level helpers (whitespace/comment skipping, symbols, keywords,
-- identifiers, literals). Nothing here depends on "C02.AST" — these are the
-- primitives the parser is built from.
module C02.Parser.Lexer
  ( Parser
  , sc
  , lexeme
  , symbol
  , keyword
  , identifier
  , intLiteralParser
  , stringLiteralParser
  , operator
  ) where

import Text.Megaparsec
import Text.Megaparsec.Char
import qualified Text.Megaparsec.Char.Lexer as L
import Data.Void (Void)
import Data.Set (Set)
import qualified Data.Set as Set
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

-- The reserved words (SPEC 1.1): the keywords, plus true/false/null — those
-- three lex to numeric literals BEFORE identifier scanning runs, so they can't
-- name anything either.
reservedNames :: Set String
reservedNames = Set.fromList
  [ "fn", "decl", "reg", "struct", "return", "if", "else", "while", "for"
  , "break", "continue", "interrupt", "asm"
  , "void", "u8", "i8", "u16", "i16"
  , "true", "false", "null"
  ]

-- a character that can continue an identifier or keyword
identChar :: Parser Char
identChar = alphaNumChar <|> char '_'

-- Keywords match against maximal identifier length (SPEC 1.1) — 'return_value'
-- must NOT match 'return'. 'try' so a partial match backtracks cleanly and the
-- caller's 'choice' can fall through to the identifier path.
keyword :: String -> Parser String
keyword keyw = lexeme . try $ string keyw <* notFollowedBy identChar

-- Parse an identifier, rejecting reserved words (SPEC 1.1/1.2). 'try' so a
-- reserved word is left unconsumed for the caller to fall through on.
identifier :: Parser String
identifier = try $ do
  name <- (:) <$> (letterChar <|> char '_') <*> many identChar
  if name `Set.member` reservedNames
    then fail ("keyword `" ++ name ++ "` cannot be used as an identifier")
    else return name

-- Parse an int literal in the various accepted formats. The radix prefixes use
-- 'string', not 'symbol' — 'symbol' runs the space consumer, letting "0x FF"
-- lex as 255. true/false/null are numeric literals 1/0/0 (SPEC 1.1).
intLiteralParser :: Parser Int
intLiteralParser = choice
  [ lexeme (numeral <* notFollowedBy alphaNumChar)
  , 1 <$ keyword "true"
  , 0 <$ keyword "false"
  , 0 <$ keyword "null"
  ]
  where
    numeral = choice
      [ string "0x" *> L.hexadecimal
      , string "0X" *> L.hexadecimal
      , string "0b" *> L.binary
      , string "0B" *> L.binary
      , L.decimal
      ]

-- A double-quoted string literal (SPEC 1.4). The recognized escapes map to
-- their control characters; any OTHER '\c' drops the backslash and keeps c
-- verbatim. A literal newline (or EOF) before the closing quote is an error —
-- strings can't span lines.
stringLiteralParser :: Parser String
stringLiteralParser = lexeme (char '"' *> manyTill strChar (char '"'))
  where
    strChar = (char '\\' *> escape) <|> satisfy (\c -> c /= '"' && c /= '\n')
    escape = choice
      [ '\n' <$ char 'n'
      , '\t' <$ char 't'
      , '\r' <$ char 'r'
      , '\0' <$ char '0'
      , anySingle          -- \\ \" \' and the drop-the-backslash fallback
      ]

-- Match an operator token, enforcing maximal munch: the same trick 'keyword'
-- uses, but guarding against operator chars instead of identifier chars. This is
-- what stops operator "<" from matching the '<' in "<<" or "<=". The inner 'try'
-- keeps a partial match (e.g. "<" of an expected "<=") non-consuming so
-- makeExprParser can fall through to another operator.
operator :: String -> Parser String
operator name = lexeme (try (string name <* notFollowedBy (oneOf opChars)))
  where opChars = "+-*/%<>=!&|^~" :: [Char]
