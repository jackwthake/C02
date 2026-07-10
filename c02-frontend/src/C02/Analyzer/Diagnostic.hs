-- | Analyzer diagnostics — the §8.1 error catalog modeled as a sum type, one
-- constructor per error kind, each carrying whatever context that error needs
-- to report itself. This grows as the analyzer covers more of §8; later each
-- kind will also carry a source span. For now it holds only what the checks
-- we've written so far can raise.
module C02.Analyzer.Diagnostic
  ( Diagnostic(..)
  ) where

import C02.Parser.AST (BaseType)

data Diagnostic
  = LiteralOutOfRange Int    -- ^ ERR_LITERAL_OUT_OF_RANGE (§3.4/§8.1): an integer
                             --   literal outside the -32768..65535 band. Carries
                             --   the offending value for the message.
  | StructCastByValue
  | UnknownStruct     String
  | UndeclaredIdentifier String
  | NotAFunction  String     -- ^ ERR_NOT_A_FUNCTION (§8.1): a 'Call' whose target
                             --   resolves to a non-function (variable) symbol.
  | NotAssignable String     -- ^ ERR_NOT_ASSIGNABLE (§8.1): a function (or struct)
                             --   name used where a value was expected — e.g. a bare
                             --   'Var' reference to a function.
  | WrongArgCount String Int Int
                             -- ^ ERR_WRONG_ARG_COUNT (§8.1): call name, expected
                             --   parameter count, actual argument count. Checked
                             --   before any argument type (§8.1).
  | TypeMismatch (BaseType, Int) (BaseType, Int) String
                             -- ^ ERR_TYPE_MISMATCH (§8.1): expected type, actual
                             --   type, and a context tag ("function call", …). The
                             --   pairs are a 'Ty' spelled out — 'Ty' lives in
                             --   C02.Analyzer.Types, which imports this module, so
                             --   naming it here would be a module cycle.
  deriving (Show, Eq)
