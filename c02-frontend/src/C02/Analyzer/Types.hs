-- | Static type relations for semantic analysis.
-- (scopes, diagnostics) is built on top of this. Everything here is pure — no
-- symbol table, no error state
module C02.Analyzer.Types
  ( Ty
  , Env(..)
  , Symbol(..)
  , Signedness(..)
  , isTypeCompatible
  , intLiteralType
  , inferType
  , checkCast
  , width
  , signedness
  ) where

import C02.Parser.AST (BaseType(..), Expr(..))
import C02.Analyzer.Diagnostic (Diagnostic(..))
import Data.Set (Set)
import qualified Data.Set as Set
import Data.Map (Map)
import qualified Data.Map as Map

-- | A fully-resolved type: a base kind paired with a pointer depth (0 means
-- "not a pointer") — a struct's name already lives inside 'BaseType'\'s 
-- 'StructName' constructor, so this pair carries everything @_type@ did 
-- (kind, ptr_depth, and the struct name).
type Ty = (BaseType, Int)

data Signedness = Signed | Unsigned
                deriving (Show, Eq)


-- | Byte width of a scalar kind (SPEC §3.2). 'Nothing' for void/struct — unlike
-- the reference, which throws: Haskell nudges partial functions toward 'Maybe',
-- so "not a scalar" is a value we can pattern-match, not a crash.
width :: BaseType -> Maybe Int
width U8  = Just 1
width I8  = Just 1
width U16 = Just 2
width I16 = Just 2
width _   = Nothing          -- Void, StructName: no width


-- | The type of an integer literal with NO surrounding context (§3.4): the
-- value alone picks the narrowest fitting type. @0@ is the null type — void at
-- pointer depth 1 — which is what lets §3.2 rule 1 treat a bare @0@ as
-- compatible with any pointer target. Outside -32768..65535 it's
-- ERR_LITERAL_OUT_OF_RANGE. Negative values reach here only via the
-- @Unary Negate (IntLit n)@ special case (§3.4); a bare literal token from the
-- lexer is always non-negative.
--
-- Each guard mirrors one row of the §3.4 table exactly (no overlapping ranges),
-- so it eyeballs against the spec directly.
intLiteralType :: Int -> Either Diagnostic Ty
intLiteralType 0 = Right (Void, 1)                            -- null type (§3.4)
intLiteralType x
  | x >= 1      && x <= 255     = Right (U8,  0)
  | x >= -128   && x <= (-1)    = Right (I8,  0)
  | x >= 256    && x <= 65535   = Right (U16, 0)
  | x >= -32768 && x <= (-129)  = Right (I16, 0)
  | otherwise                   = Left (LiteralOutOfRange x)


-- | Signedness of a scalar kind (SPEC §3.2). 'Nothing' for void/struct.
signedness :: BaseType -> Maybe Signedness
signedness U8  = Just Unsigned
signedness U16 = Just Unsigned
signedness I8  = Just Signed
signedness I16 = Just Signed
signedness _   = Nothing


-- | Is this base kind a (named) struct?
isStruct :: BaseType -> Bool
isStruct (StructName _) = True
isStruct _              = False


-- | SPEC §3.2 @is_types_compatible@, rules 2–8, in order — the ordering is
-- load-bearing (rule 2 before 6; rules 5 < 6 < 7). Rule 1 (bare literal @0@
-- compatible with any pointer target) is /not/ here: it depends on the
-- literal's value, not on two types, so it belongs at literal-typing time.
--
-- Each guard below is one rule; a guard is the direct analog of the reference's
-- sequential @if (...) return ...@ — the first one whose condition holds wins.
isTypeCompatible :: Ty -> Ty -> Bool
isTypeCompatible (expKind, expDepth) (actKind, actDepth)
  -- Rule 2: void* accepts any pointee kind, but only at matching depth 1 (S-18).
  | expKind == Void && expDepth == 1 && actDepth == 1 = True

  -- Rule 3: pointer-ness must agree — one side a pointer, the other not, never.
  | (expDepth > 0) /= (actDepth > 0)                  = False

  -- Rule 4: both pointers, but depths differ (u8* vs u8**).
  | expDepth /= actDepth                              = False

  -- Rule 5: two structs are compatible iff their names match exactly. Derived
  -- 'Eq' on 'BaseType' compares the constructor /and/ its name, so this one
  -- '==' replaces the reference's explicit @.name@ comparison.
  | isStruct expKind && isStruct actKind              = expKind == actKind

  -- Rule 6: exact kind match (covers void<->void and any scalar<->itself).
  | expKind == actKind                                = True

  -- Rule 7: a struct or void on either side, unmatched above, is incompatible.
  | isStruct expKind || expKind == Void
    || isStruct actKind || actKind == Void            = False

  -- Rule 8, pointer case: two distinct scalar kinds still at pointer depth
  -- (same depth, mismatched pointee) — no pointee-widening between pointers (S-19).
  | expDepth /= 0 || actDepth /= 0                    = False

  -- Rule 8, non-pointer case: compatible iff signedness matches and @actual@
  -- fits within @expected@'s width (implicit widening only). The pattern guards
  -- bind the scalar info; both sides are guaranteed scalar by rules 5–7 above,
  -- so these 'Just' matches always succeed here.
  | Just es <- signedness expKind, Just as <- signedness actKind
  , Just ew <- width expKind,      Just aw <- width actKind
  = es == as && aw <= ew

  -- Unreachable given the rules above, but keeps the function total (no -Wall
  -- "non-exhaustive guards" warning, and no surprise crash if a rule changes).
  | otherwise = False


-- | What a name resolves to. A single 'Ty' can't describe every name: a
-- function needs a return type /and/ its parameter types, which a variable
-- doesn't have — so the stored value is a sum, one constructor per symbol kind.
data Symbol
  = VarSym  Ty          -- ^ a variable of this type
  | FuncSym Ty [Ty]     -- ^ a function: return type, then parameter types
  deriving (Show, Eq)

data Env = Env
  { structNames :: Set String         -- checkCast's unknown-struct check
  , symbols     :: Map String Symbol  -- Var / Call lookups
  }


-- | Resolve an expression's type (SPEC §3/§6.3). STUB for now: only the two
-- constructors 'checkCast' actually needs are real. Every other expression
-- returns a placeholder @(U8,0)@ so the module compiles and casts are testable;
-- fill these in one constructor at a time ('Var' next → the symbol table). The
-- placeholder never escapes a cast, since a cast discards its source type.
inferType :: Env -> Expr -> Either Diagnostic Ty
inferType _   (IntLit n)         = intLiteralType n
inferType env (Cast bt d e)      = checkCast env bt d e
-- A bare identifier must be a variable; a function name here is a value misuse.
inferType env (Var name)         = case symbolLookup env name of
  Left err            -> Left err
  Right (VarSym ty)   -> Right ty
  Right (FuncSym _ _) -> Left (NotAssignable name)         -- ERR_NOT_ASSIGNABLE
-- A call's type is the callee's RETURN type; a variable here can't be called.
inferType env (Call name args)   = case symbolLookup env name of
  Left err                   -> Left err
  Right (VarSym _)           -> Left (NotAFunction name)   -- ERR_NOT_A_FUNCTION
  Right (FuncSym ret params) -> checkArgs env name args params ret
inferType _   _                  = Right (U8, 0)          -- STUB: unhandled exprs


-- | Validate a call's arguments (SPEC §6/§8.1). Arg COUNT is checked before any
-- type (§8.1); then each argument's inferred type must be compatible with the
-- matching parameter type — @isTypeCompatible param arg@, param is "expected".
-- Reports the first problem and stops (single-error, like the rest of the
-- analyzer for now); on success the call's type is the function's return type.
checkArgs :: Env -> String -> [Expr] -> [Ty] -> Ty -> Either Diagnostic Ty
checkArgs env name args params ret
  | length args /= length params =
      Left (WrongArgCount name (length params) (length args))
  | otherwise = go (zip args params)
  where
    go [] = Right ret                          -- every arg checked out
    go ((arg, paramTy) : rest) =
      case inferType env arg of
        Left err -> Left err                   -- the arg itself failed to type
        Right argTy
          | isTypeCompatible paramTy argTy -> go rest
          | otherwise -> Left (TypeMismatch paramTy argTy "function call")


-- | The shared name-resolution step: does this name resolve to a symbol at all?
-- 'Var' and 'Call' both start here, then branch on the returned symbol's kind.
symbolLookup :: Env -> String -> Either Diagnostic Symbol
symbolLookup env name = case Map.lookup name (symbols env) of
    Just sym -> Right sym
    Nothing  -> Left (UndeclaredIdentifier name)           -- ERR_UNDECLARED_IDENTIFIER


-- | SPEC §3.3 cast typing. cc02 order (analyzer.c:398): resolve the source
-- first (only to surface errors /inside/ the operand — its type is then thrown
-- away), then reject an unknown struct target (any depth), then a by-value
-- struct target (depth 0). Any other destination is accepted with no
-- relatedness check: the result type is just the destination.
--
--           Struct set  Dest kind   Dest depth  Source expr  Error | Type
checkCast :: Env      -> BaseType -> Int      -> Expr      -> Either Diagnostic Ty
checkCast env destBt destDepth src =
  case inferType env src of                    -- resolve source; discard its type
    Left err -> Left err
    Right _  -> checkDest
  where
    checkDest
      | StructName n <- destBt
      , n `Set.notMember` structNames env   = Left (UnknownStruct n)   -- any depth
      | isStruct destBt && destDepth == 0   = Left StructCastByValue   -- by value
      | otherwise                           = Right (destBt, destDepth)