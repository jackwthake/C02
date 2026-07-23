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
  , isLvalue
  , width
  , signedness
  , isPtr
  ) where

import C02.Parser.AST
  ( BaseType(..), Expr(..), NamedType
  , BinOp (And, Or, Sub, Add)
  , UnOp (AddressOf, Incr, Decr, Bang, BitNot, Negate) )
import C02.Analyzer.Diagnostic (Diagnostic(..))
import Data.Map (Map)
import qualified Data.Map as Map
import Data.Maybe (isJust)

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

wider :: Ty -> Ty -> Ty
wider (l_bt, l_d) (r_bt, r_d)
  -- Every pointer is 2 bytes on the 65c02 regardless of pointee (codegen
  -- never sizes by pointee kind), so "wider" is meaningless between two
  -- pointers — comparing width l_bt/r_bt here would compare POINTEE widths
  -- (e.g. u8* vs u16* -> 1 vs 2), which is a different, wrong question. This
  -- branch exists only to dodge that comparison, not to answer it: when the
  -- two pointers are the same type it's a don't-care (either side is right);
  -- when they're merely compatible-but-different (e.g. void* meets u8* via
  -- rule 2), picking @l@ is an arbitrary tie-break, not a spec-derived
  -- answer — §6.3 doesn't say which pointee kind should win that case.
  | l_d > 0 && r_d > 0 = (l_bt, l_d)
  | l_w >= r_w         = (l_bt, l_d)
  | l_w <= r_w         = (r_bt, r_d)
  | otherwise          = (l_bt, l_d)
  where 
    l_w = width l_bt
    r_w = width r_bt


-- see width - only int types return a width, everything else just maybe
isInt :: Ty -> Bool
isInt (t, d) = d == 0 && isJust (width t)


isPtr :: Ty -> Bool
isPtr (_, d)
  | d > 0    = True
isPtr (_, _) = False

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


-- | SPEC §3.2 @is_types_compatible@, rules 1–8, in order — the ordering is
-- load-bearing (rule 1 before all; rule 2 before 6; rules 5 < 6 < 7). Each guard
-- below is one rule, the direct analog of the reference's sequential
-- @if (...) return ...@ — the first one whose condition holds wins.
--
-- Rule 1 note: SPEC's intended scope is pointer-only (the null literal @0@,
-- typed @void@ at ptr_depth 1, satisfies only a pointer destination). But the
-- type system can't tell the literal @0@/@null@ from a genuine @void*@ value —
-- both are @(Void, 1)@ — so we take cc02's relaxed reading (S-1/S-17): a
-- null-shaped /actual/ satisfies /any/ destination. This is what makes
-- @u8 x = 0;@ and @u8 x = null;@ well-formed (deliberately relaxed from spec).
isTypeCompatible :: Ty -> Ty -> Bool
isTypeCompatible (expKind, expDepth) (actKind, actDepth)
  -- Rule 1 (relaxed): a null-shaped actual (0/null, void at depth 1) is
  -- compatible with anything — pointer or scalar destination alike.
  | actKind == Void && actDepth == 1                  = True

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
  = VarSym  Ty           -- ^ a variable of this type
  | FuncSym Ty [Ty] Bool -- ^ a function: return type, then parameter types, isInterrupt
  deriving (Show, Eq)

data Env = Env
  { structDefs :: Map String [NamedType]  -- struct name -> fields; membership is
                                          -- the unknown-struct check, fields feed
                                          -- (later) Field / StructInit typing
  , symbols    :: Map String Symbol       -- Var / Call lookups
  , registers  :: Map String Int          -- register name -> fixed address. Lowering
                                          -- only: a read/write of one of these names
                                          -- becomes a LOAD/STORE at the address rather
                                          -- than a var copy. Empty during analysis
                                          -- (registers type as ordinary 'VarSym').
  } deriving (Show)


-- | Resolve an expression's type (SPEC §3/§6.3/§6.4/§5.2).
inferType :: Env -> Expr -> Either Diagnostic Ty
inferType _   (IntLit n)         = intLiteralType n
inferType env (Cast bt d e)      = checkCast env bt d e
-- A bare identifier must be a variable; a function name here is a value misuse.
inferType env (Var name)         = case symbolLookup env name of
  Left err            -> Left err
  Right (VarSym ty)   -> Right ty
  Right (FuncSym _ _ _) -> Left (NotAssignable name)         -- ERR_NOT_ASSIGNABLE
-- A call's type is the callee's RETURN type; a variable here can't be called.
inferType env (Call name args)   = case symbolLookup env name of
  Left err                         -> Left err
  Right (VarSym _)                 -> Left (NotAFunction name)   -- ERR_NOT_A_FUNCTION
  Right (FuncSym ret params i) -> case i of
    True  -> Left (InterruptCall name)
    False -> checkArgs env name args params ret
-- Resolve left, then right (first error wins), then apply §6.3's rules.
inferType env (Binary op l r)    = case inferType env l of
  Left err -> Left err
  Right lTy -> case inferType env r of
    Left err -> Left err
    Right rTy -> checkBinary op lTy rTy
-- A string literal is a char pointer (§3): u8 at pointer depth 1.
inferType _   (StrLit _)         = Right (U8, 1)
-- Unary. §3.4: a NEGATED integer literal is typed from its negative value
-- (so -5 is i8, not u8) — this is the only place a negative literal's type is
-- decided, since the lexer emits only non-negative literals. Must precede the
-- general Negate case.
inferType _   (Unary Negate (IntLit n)) = intLiteralType (negate n)
inferType env (Unary Negate e)   = inferType env e
inferType env (Unary BitNot e)   = inferType env e
inferType env (Unary Bang e)     = inferType env e >> Right (U8, 0)   -- boolean result
-- &e requires an lvalue; the result is one pointer level deeper. Resolve the
-- operand first (so an error inside it wins over the lvalue complaint, matching
-- resolve order), then apply the lvalue rule (§7.3).
inferType env (Unary AddressOf e) = case inferType env e of
  Left err          -> Left err
  Right (bt, d)
    | isLvalue e    -> Right (bt, d + 1)
    | otherwise     -> Left (NotLvalue "cannot take the address of a non-lvalue")
-- ++e / --e keep the operand's type but require an lvalue (§7.3).
inferType env (Unary Incr e)     = incrDecr env "increment" e
inferType env (Unary Decr e)     = incrDecr env "decrement" e
-- *e peels one pointer level; dereferencing a non-pointer is ERR_TYPE_MISMATCH
-- with context "dereference" (expected shows the pointer it should have been).
inferType env (Deref e)          = case inferType env e of
  Left err       -> Left err
  Right (bt, d)
    | d > 0      -> Right (bt, d - 1)
    | otherwise  -> Left (TypeMismatch (bt, 1) (bt, d) "dereference")
-- a.b (§6.4): resolve the base, auto-deref exactly one level if it's a
-- Struct* (a Struct** base is untouched, so the next check rejects it), then
-- the (possibly-adjusted) type must be a bare struct, registered, with the
-- named field.
inferType env (Field baseExpr fname) = case inferType env baseExpr of
  Left err     -> Left err
  Right baseTy0 -> case autoDerefStruct baseTy0 of
    (StructName sn, 0) -> case Map.lookup sn (structDefs env) of
      Nothing     -> Left (UnknownStruct sn)
      Just fields -> case [ (bt, d) | (bt, d, n) <- fields, n == fname ] of
        (fty : _) -> Right fty
        []        -> Left (UnknownField sn fname)
    baseTy -> Left (NotAStruct fname baseTy)
-- Name{ .f = e, ... } (§5.2): the target struct must be registered; each
-- given field must exist on it and its value compatible with the field's
-- type (S-6: omitted fields and duplicate entries are not diagnosed). The
-- result is the struct type itself, at depth 0.
inferType env (StructInit sname inits)
  | sname `Map.notMember` structDefs env = Left (UnknownStruct sname)
  | otherwise = checkInits (structDefs env Map.! sname) inits >> Right (StructName sname, 0)
  where
    checkInits _      []                     = Right ()
    checkInits fields ((fname, val) : rest)  =
      case [ (bt, d) | (bt, d, n) <- fields, n == fname ] of
        [] -> Left (UnknownField sname fname)
        (fty : _) -> case inferType env val of
          Left err  -> Left err
          Right vty
            | isTypeCompatible fty vty -> checkInits fields rest
            | otherwise -> Left (TypeMismatch fty vty fname)

-- | Peel exactly one pointer level off a @Struct*@ type (§6.4's auto-deref);
-- any other type (already a bare struct, a scalar, or @Struct**@+) passes
-- through unchanged so the caller's "is this now a bare struct?" check
-- rejects it uniformly.
autoDerefStruct :: Ty -> Ty
autoDerefStruct (StructName sn, 1) = (StructName sn, 0)
autoDerefStruct ty                 = ty


-- | Shared body for ++e / --e: resolve the operand, then demand it be an lvalue.
incrDecr :: Env -> String -> Expr -> Either Diagnostic Ty
incrDecr env verb e = case inferType env e of
  Left err            -> Left err
  Right ty
    | isLvalue e      -> Right ty
    | otherwise       -> Left (NotLvalue ("cannot " ++ verb ++ " a non-lvalue"))


-- | An lvalue (assignable / addressable, §7.3) is a named variable, a struct
-- field, or a pointer dereference — never a literal, call result, or computed
-- value. Used both by expression typing (&, ++, --) and by the assignment
-- statement check.
isLvalue :: Expr -> Bool
isLvalue (Var _)     = True
isLvalue (Field _ _) = True
isLvalue (Deref _)   = True
isLvalue _           = False


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
      , n `Map.notMember` structDefs env    = Left (UnknownStruct n)   -- any depth
      | StructName n <- destBt, destDepth == 0 = Left (StructCastByValue n)  -- by value
      | otherwise                           = Right (destBt, destDepth)


checkBinary :: BinOp -> Ty -> Ty -> Either Diagnostic Ty
checkBinary op l r
  | op == Add && isInt l && isPtr r              = Right (r)
  | op == Add && isPtr l && isInt r              = Right (l)
  | op == Sub && isPtr l && isInt r              = Right (l)
  | op `elem` [And, Or]                          = Right (U8, 0)
  | isTypeCompatible l r || isTypeCompatible r l = Right (wider l r)
  | otherwise = Left (TypeMismatch l r "binary operation")