-- | Analyzer diagnostics — the §8 error catalog modeled as a sum type, one
-- constructor per error kind, each carrying whatever context that error needs
-- to report itself. 'render' turns a diagnostic into its SPEC-worded message
-- body (no location yet — the span retrofit will prepend @file:line:col@).
module C02.Analyzer.Diagnostic
  ( Diagnostic(..)
  , Diag(..)
  , render
  ) where

import C02.Parser.AST (BaseType(..))

-- | A resolved type spelled out as (kind, pointer depth). This is the same shape
-- as @C02.Analyzer.Types.Ty@, respelled here because that module imports this
-- one — naming 'Ty' here would be an import cycle.
type TyPair = (BaseType, Int)

data Diagnostic
  = LiteralOutOfRange Int      -- ^ ERR_LITERAL_OUT_OF_RANGE (§3.4): literal outside -32768..65535
  | StructCastByValue String   -- ^ ERR_STRUCT_CAST_BY_VALUE: @(Struct)expr@ with no pointer level
  | UnknownStruct String       -- ^ ERR_UNKNOWN_STRUCT: struct-typed name not registered
  | UndeclaredIdentifier String-- ^ ERR_UNDECLARED_IDENTIFIER: name not in any visible scope
  | NotAFunction String        -- ^ ERR_NOT_A_FUNCTION: call target is a non-function symbol
  | NotAssignable String       -- ^ ERR_NOT_ASSIGNABLE: function/struct name used as a value
  | WrongArgCount String Int Int -- ^ ERR_WRONG_ARG_COUNT: name, expected count, actual count
  | TypeMismatch TyPair TyPair String -- ^ ERR_TYPE_MISMATCH: expected, actual, context tag
  | Redeclaration String       -- ^ ERR_REDECLARATION: name inserted twice into one scope frame (§7.2)
  | ShadowedDeclaration String -- ^ ERR_SHADOWED_DECLARATION: local/param reuses an enclosing name (§7.2)
  | MissingMain                -- ^ ERR_MISSING_MAIN: no function definition named @main@
  | MissingReturn String       -- ^ ERR_MISSING_RETURN: non-void fn whose last stmt can't be seen to return
  | NotLvalue String           -- ^ ERR_NOT_LVALUE: carries the full message (varies by &/++/--/assign)
  | VoidVariable String        -- ^ ERR_VOID_VARIABLE: non-pointer void as a variable/param/field/global
  | BreakOutsideLoop           -- ^ ERR_BREAK_OUTSIDE_LOOP
  | ContinueOutsideLoop        -- ^ ERR_CONTINUE_OUTSIDE_LOOP
  | IncompleteStructField String String -- ^ ERR_INCOMPLETE_STRUCT_FIELD (§4.4): struct name, referenced (incomplete) struct name — equal when self-referential
  | UnknownField String String -- ^ ERR_UNKNOWN_FIELD (§6.4/§5.2): struct name, field name not found on it
  | NotAStruct String TyPair   -- ^ ERR_NOT_A_STRUCT (§6.4): field name, actual (post-auto-deref) type of the base
  deriving (Show, Eq)


-- | A diagnostic paired with where (if anywhere) it points. 'At' carries the
-- source byte offset of the statement/declaration it arose in, for a
-- @file:line:col@ + caret render; 'Free' is a whole-translation-unit diagnostic
-- with no meaningful span (only 'MissingMain' so far), rendered as a plain line.
data Diag
  = At !Int Diagnostic
  | Free Diagnostic
  deriving (Show, Eq)


-- | The message body for a diagnostic, matching SPEC §8 wording. Location and
-- the leading @error:@ tag are added later by the renderer that owns spans.
render :: Diagnostic -> String
render d = case d of
  LiteralOutOfRange _     -> "integer literal is out of range (must fit in -32768..65535)"
  StructCastByValue n     -> "cannot cast to struct '" ++ n ++ "' by value; cast to '" ++ n ++ "*' instead"
  UnknownStruct n         -> "unknown struct '" ++ n ++ "'"
  UndeclaredIdentifier n  -> "undeclared identifier '" ++ n ++ "'"
  NotAFunction n          -> "'" ++ n ++ "' is not a function"
  NotAssignable n         -> "'" ++ n ++ "' is not assignable"
  WrongArgCount n e a     -> "function '" ++ n ++ "' expects " ++ show e
                             ++ " argument(s), but " ++ show a ++ " were provided"
  TypeMismatch e a ctx    -> "type mismatch in " ++ ctx
                             ++ ": expected " ++ renderTy e ++ ", found " ++ renderTy a
  Redeclaration n         -> "redeclaration of '" ++ n ++ "' in this scope"
  ShadowedDeclaration n   -> "declaration of '" ++ n
                             ++ "' shadows an outer-scope declaration "
                             ++ "(shadowing is not supported - rename one of them)"
  MissingMain             -> "missing or improperly defined main function"
  MissingReturn n         -> "non-void function '" ++ n
                             ++ "' may reach its end without returning a value"
  NotLvalue msg           -> msg
  VoidVariable n          -> "'" ++ n ++ "' declared with incomplete type 'void'"
  BreakOutsideLoop        -> "'break' statement not within a loop"
  ContinueOutsideLoop     -> "'continue' statement not within a loop"
  IncompleteStructField s f
    | s == f              -> "struct '" ++ s ++ "' cannot contain itself by value; use a pointer"
    | otherwise            -> "struct '" ++ s ++ "' has field of incomplete type '" ++ f
                               ++ "' (not yet declared)"
  UnknownField s f        -> "struct '" ++ s ++ "' has no field '" ++ f ++ "'"
  NotAStruct f actual     -> "request for field '" ++ f ++ "' in '" ++ renderTy actual
                             ++ "', which is not a struct"


-- | Render a type the way §8 diagnostics spell it: base kind plus one @*@ per
-- pointer level (@u8@, @u8*@, @u16**@, @Point@).
renderTy :: TyPair -> String
renderTy (bt, depth) = base ++ replicate depth '*'
  where
    base = case bt of
      U8           -> "u8"
      I8           -> "i8"
      U16          -> "u16"
      I16          -> "i16"
      Void         -> "void"
      StructName n -> n
