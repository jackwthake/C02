-- | The semantic-analysis driver (SPEC §7): the walk that ties the pure type
-- relations in "C02.Analyzer.Types" to a whole 'Program'. Two conceptual passes:
--
--   * Pass 1 ('buildGlobals') registers every top-level declaration into the
--     global scope, so forward references resolve, and reports redeclarations.
--   * Pass 2 ('analyzeFunc') walks each function body, threading a scope stack,
--     the enclosing return type, and a loop depth.
--
-- Three Haskell idioms carry the design:
--
--   * __Accumulate, don't short-circuit.__ The pure layer returns
--     @Either Diagnostic Ty@ (first error wins); the walk runs in
--     @ReaderT Ctx (Writer [Diag])@ so a failed sub-check contributes a
--     diagnostic and the walk keeps going. 'infer' bridges the two, turning a
--     'Left' into an emitted diagnostic plus a 'Nothing' "poison" — poison is
--     just 'Maybe', so downstream checks skip an already-broken operand.
--
--   * __Scope is 'local', not push/pop.__ Entering a block runs the rest of the
--     walk under an extended 'Ctx'; returning from that sub-computation /is/ the
--     pop. A local declaration's scope is literally the recursive
--     @analyzeBlock rest@ it wraps, so nothing leaks past the block.
--
--   * __Location travels in the context, not the pure layer.__ Each statement
--     and top-level declaration carries a source offset ('Loc'); the walk sets
--     'ctxOffset' as it enters one, and 'emit' stamps every diagnostic with the
--     current offset. So diagnostics get a @file:line:col@ position without the
--     offset-blind 'inferType' ever having to know about it.
module C02.Analyzer.Analyze
  ( analyze
  , bindLocal
  ) where

import           Control.Monad (forM_, unless, void, when)
import           Control.Monad.Reader (ReaderT, asks, local, runReaderT)
import           Control.Monad.Writer (Writer, execWriter, tell)
import           Data.Map (Map)
import qualified Data.Map as Map
import qualified Data.Set as Set

import           C02.Parser.AST
import           C02.Analyzer.Diagnostic (Diagnostic(..), Diag(..))
import           C02.Analyzer.Layout (computeStructLayouts)
import           C02.Analyzer.Types
                   ( Env(..), Symbol(..), Ty
                   , inferType, isTypeCompatible, isLvalue )


-- | One lexical scope frame: names visible at that nesting level.
type Scope = Map String Symbol

-- | Everything pass 2 threads through the walk. The scope list has the innermost
-- frame at its head and the global frame at its tail (never empty).
data Ctx = Ctx
  { ctxScopes     :: [Scope]                -- innermost first, global last
  , ctxStructs    :: Map String [NamedType] -- struct definitions visible at this scope (order-sensitive for locals)
  , ctxAllStructs :: Set.Set String         -- every struct name declared anywhere in the program, any order/scope
                                            -- (SPEC's "otherwise fully forward-reference-tolerant symbol table") —
                                            -- static for the whole walk, only used to validate a LOCAL struct's own
                                            -- field types (see 'declLocalStruct'): declaring a variable of a struct
                                            -- type still goes through 'ctxStructs' and stays order-sensitive
  , ctxReturn     :: Ty                     -- enclosing function's return type
  , ctxLoopDepth  :: Int                    -- break/continue legal iff > 0
  , ctxOffset     :: Int                    -- source offset of the current stmt/decl
  }

type Analyze = ReaderT Ctx (Writer [Diag])


-- | Run both passes over a program and return every diagnostic. Ordering within
-- the list is by pass (pass-1 redeclarations, then top-level validation, then
-- missing-main, then pass 2); the renderer sorts the located ones by offset.
analyze :: Program -> [Diag]
analyze prog@(TopLevels decls) = p1errs ++ layoutErrs ++ execWriter (runReaderT walk ctx0)
  where
    (globalScope, structs, p1errs) = buildGlobals decls
    -- The offsets/sizes in the layout map aren't needed here — its
    -- diagnostics, and the set of every struct name it saw (regardless of
    -- scope or order), are. IR lowering will call 'computeStructLayouts'
    -- again once it needs the offsets; the recompute is pure and cheap, and
    -- keeps 'analyze's diagnostics-only contract.
    (allLayouts, layoutErrs) = computeStructLayouts prog
    ctx0 = Ctx { ctxScopes     = [globalScope]
               , ctxStructs    = structs
               , ctxAllStructs = Map.keysSet allLayouts
               , ctxReturn     = (Void, 0)
               , ctxLoopDepth  = 0
               , ctxOffset     = 0 }
    walk = do
      validateTopLevel decls
      checkMain decls
      analyzeFuncs decls


-- ---------------------------------------------------------------------------
-- Small monad helpers
-- ---------------------------------------------------------------------------

-- | Emit a diagnostic stamped with the current statement/declaration offset.
emit :: Diagnostic -> Analyze ()
emit d = do
  off <- asks ctxOffset
  tell [At off d]

-- | Run a sub-walk with the current offset set to a statement/declaration's.
withOffset :: Int -> Analyze a -> Analyze a
withOffset off = local (\c -> c { ctxOffset = off })

-- | Flatten the scope stack into the 'Env' the pure typer consumes. Shadowing is
-- disallowed (see 'declLocal'), so the union is conflict-free; 'Map.unions' is
-- left-biased and the innermost frame is leftmost, the right tie-break anyway.
envOf :: Ctx -> Env
envOf c = Env (ctxStructs c) (Map.unions (ctxScopes c)) Map.empty  -- no registers during analysis

-- | Resolve an expression's type in the current context, bridging the pure
-- @Either@ typer into the accumulating walk: on an error, emit it (at the
-- current offset) and yield 'Nothing' (poison) so callers skip the broken
-- operand.
infer :: Expr -> Analyze (Maybe Ty)
infer e = do
  env <- asks envOf
  case inferType env e of
    Left d  -> emit d >> pure Nothing
    Right t -> pure (Just t)

-- | Check an inferred (maybe-poison) type against an expected one, emitting a
-- 'TypeMismatch' tagged with @ctx@ on a real incompatibility. A poisoned actual
-- is silent — its error was already reported.
expectCompat :: Ty -> Maybe Ty -> String -> Analyze ()
expectCompat _        Nothing       _   = pure ()
expectCompat expected (Just actual) ctx
  | isTypeCompatible expected actual     = pure ()
  | otherwise                            = emit (TypeMismatch expected actual ctx)

-- | Push a fresh empty scope for the duration of the given sub-walk.
pushScope :: Ctx -> Ctx
pushScope c = c { ctxScopes = Map.empty : ctxScopes c }

-- | Enter a loop body: bump the depth so break/continue become legal.
incLoop :: Ctx -> Ctx
incLoop c = c { ctxLoopDepth = ctxLoopDepth c + 1 }

-- | Bind a name into the innermost scope (assumes shadow/redecl already checked).
bindLocal :: String -> Symbol -> Ctx -> Ctx
bindLocal n s c = case ctxScopes c of
  (top : rest) -> c { ctxScopes = Map.insert n s top : rest }
  []           -> c { ctxScopes = [Map.singleton n s] }  -- unreachable: stack never empty


-- ---------------------------------------------------------------------------
-- Declaration checks (shared by locals and parameters)
-- ---------------------------------------------------------------------------

-- | Validate a type appearing in a variable/parameter/field/global declaration:
-- non-pointer @void@ is rejected (§7.4), and a struct type must satisfy the
-- given "is this struct known" predicate. Returns whether the type is legal;
-- on a bad type the caller still binds the name (so later uses don't
-- re-report) but skips the init check.
checkDeclTypeWith :: (String -> Analyze Bool) -> String -> Ty -> Analyze Bool
checkDeclTypeWith isKnownStruct name (bt, depth)
  | bt == Void && depth == 0 = emit (VoidVariable name) >> pure False
  | StructName sn <- bt = do
      known <- isKnownStruct sn
      if known then pure True else emit (UnknownStruct sn) >> pure False
  | otherwise = pure True

-- | 'checkDeclTypeWith' against the current scope's visible structs
-- ('ctxStructs') — order-sensitive for a local struct declared earlier in the
-- same block, same as any other local declaration.
checkDeclType :: String -> Ty -> Analyze Bool
checkDeclType = checkDeclTypeWith (\sn -> asks (Map.member sn . ctxStructs))

-- | Reject a declaration that reuses a name (§7.2): same frame → redeclaration,
-- an outer live frame → shadowing (disallowed, unlike C).
checkShadowRedecl :: String -> Analyze ()
checkShadowRedecl name = do
  scopes <- asks ctxScopes
  case scopes of
    (top : outer)
      | Map.member name top             -> emit (Redeclaration name)
      | any (Map.member name) outer     -> emit (ShadowedDeclaration name)
    _                                   -> pure ()

-- | Declare a local variable at offset @off@, then run the continuation with it
-- in scope. This is the "scope is 'local'" idiom: the binding is visible for
-- exactly @k@ (the rest of the block) and vanishes when @k@ returns. The
-- declaration's own checks run under @off@; @k@ sets its own statements' offsets.
declLocal :: Int -> VarDecl -> Analyze a -> Analyze a
declLocal off vd k = do
  let ty   = (varType vd, ptrDepth vd)
      name = declName vd
  withOffset off $ do
    ok <- checkDeclType name ty
    checkShadowRedecl name
    forM_ (declInit vd) $ \e -> do
      actual <- infer e
      when ok (expectCompat ty actual name)   -- init context tag is the variable name
  local (bindLocal name (VarSym ty)) k

-- | Declare a function parameter (like 'declLocal' but no initializer). Params
-- share the function-body scope, so they carry the function's offset.
declParam :: NamedType -> Analyze a -> Analyze a
declParam (bt, depth, name) k = do
  _ <- checkDeclType name (bt, depth)
  checkShadowRedecl name
  local (bindLocal name (VarSym (bt, depth))) k


-- ---------------------------------------------------------------------------
-- Pass 1: register globals
-- ---------------------------------------------------------------------------

-- | Fold the top-level declarations into the global scope and struct table,
-- reporting a redeclaration (at the offending declaration's offset) the second
-- time any name appears. Structs, globals, registers and functions share one
-- namespace (SPEC §7.2), so a single "seen" set guards them all — including a
-- same-file @decl@ that duplicates a later definition (SPEC S-7: @decl@ is
-- cross-file only, never an in-file prototype).
buildGlobals :: [Loc TopLevelDecl] -> (Scope, Map String [NamedType], [Diag])
buildGlobals = go Map.empty Map.empty Set.empty []
  where
    go syms structs _    errs [] = (syms, structs, reverse errs)
    go syms structs seen errs (Loc off d : ds)
      | Set.member name seen = go syms structs seen (At off (Redeclaration name) : errs) ds
      | otherwise =
          let syms'    = maybe syms    (\s  -> Map.insert name s  syms)    mSym
              structs' = maybe structs (\fs -> Map.insert name fs structs) mStruct
          in go syms' structs' (Set.insert name seen) errs ds
      where (name, mSym, mStruct) = classify d

    classify :: TopLevelDecl -> (String, Maybe Symbol, Maybe [NamedType])
    classify (GlobalVarDecl v) = (declName v, Just (VarSym (varType v, ptrDepth v)), Nothing)
    classify (FwdVarDecl v)    = (declName v, Just (VarSym (varType v, ptrDepth v)), Nothing)
    classify (RegisterDecl r)  = (regName r,  Just (VarSym (regType r, regPtrDepth r)), Nothing)
    classify (FunctionDecl f)  = (funcName f, Just (funcSym f), Nothing)
    classify (FwdFuncDecl f)   = (funcName f, Just (funcSym f), Nothing)
    classify (StructDef s)     = (structName s, Nothing, Just (structFields s))
    classify (IncludeStmt _)   = error "classify: IncludeStmt reached the analyzer (resolveIncludes should strip all includes first)"

    funcSym f = FuncSym (funcReturnType f, funcReturnPtrDepth f)
                        [ (bt, depth) | (bt, depth, _) <- params f ]


-- | After pass 1, validate the types written in top-level declarations (now that
-- every struct name is registered, forward references resolve) and type-check
-- global initializers in the global scope. Each declaration's checks run under
-- its own offset.
validateTopLevel :: [Loc TopLevelDecl] -> Analyze ()
validateTopLevel = mapM_ (\(Loc off d) -> withOffset off (one d))
  where
    one (IncludeStmt _) = error "validateTopLevel: IncludeStmt reached the analyzer (resolveIncludes should strip all includes first)"
    one (GlobalVarDecl v) = do
      ok <- checkDeclType (declName v) (varType v, ptrDepth v)
      forM_ (declInit v) $ \e -> do
        actual <- infer e
        when ok (expectCompat (varType v, ptrDepth v) actual (declName v))
    one (FwdVarDecl v)   = void $ checkDeclType (declName v) (varType v, ptrDepth v)
    one (RegisterDecl r) = void $ checkDeclType (regName r) (regType r, regPtrDepth r)
    one (StructDef s)    = forM_ (structFields s) $ \(bt, d, nm) ->
                             void $ checkDeclType nm (bt, d)
    one (FunctionDecl f) = validateReturn f
    one (FwdFuncDecl f)  = do
      validateReturn f
      forM_ (params f) $ \(bt, d, nm) -> void $ checkDeclType nm (bt, d)

    -- A function return type may legitimately be void; only an unknown struct is
    -- an error here (parameters are validated when the body is walked).
    validateReturn f = case funcReturnType f of
      StructName sn -> do
        known <- asks (Map.member sn . ctxStructs)
        unless known (emit (UnknownStruct sn))
      _ -> pure ()


checkMain :: [Loc TopLevelDecl] -> Analyze ()
checkMain decls = forM_ mains checkOne
  where
    -- step 1: collect every top-level function named "main", with its offset
    mains = [ (off, f) | Loc off (FunctionDecl f) <- decls, funcName f == "main" ]

    -- step 2: check one of them
    checkOne (off, f) =
      withOffset off $
        unless (validMainSig f) $
          emit (BadMainSignature (funcName f))

    -- the actual rule: void return, no params
    validMainSig f = (funcReturnType f, funcReturnPtrDepth f) == (Void, 0)
                  && null (params f)


-- ---------------------------------------------------------------------------
-- Pass 2: walk function bodies
-- ---------------------------------------------------------------------------

-- | Walk every function definition, each under its own offset.
analyzeFuncs :: [Loc TopLevelDecl] -> Analyze ()
analyzeFuncs = mapM_ go
  where
    go (Loc off (FunctionDecl f)) = withOffset off (analyzeFunc f)
    go _                          = pure ()

-- | Walk one function: its parameters and top-level body locals share a single
-- scope (a body local reusing a parameter name is therefore a redeclaration).
-- After the body, a non-void function whose last statement can't be seen to
-- return is flagged (§7.5, shallow) at the function's offset.
analyzeFunc :: FuncDecl -> Analyze ()
analyzeFunc f =
  local (\c -> (pushScope c) { ctxReturn = (funcReturnType f, funcReturnPtrDepth f) }) $
    bindParams (params f) $ do
      analyzeBlock stmts
      checkMissingReturn f stmts
  where
    stmts = funcBodyStmts f

-- | Declare each parameter in turn, threading them into the body walk.
bindParams :: [NamedType] -> Analyze a -> Analyze a
bindParams []       k = k
bindParams (p : ps) k = declParam p (bindParams ps k)

-- | A function body is stored as a @Just (Nested block)@; anything else has no
-- statements to walk.
funcBodyStmts :: FuncDecl -> [Loc Stmt]
funcBodyStmts f = case body f of
  Just (Nested b) -> b
  _               -> []

-- | Walk a block, setting each statement's offset and threading each local
-- declaration into the tail via the "scope is 'local'" idiom.
analyzeBlock :: [Loc Stmt] -> Analyze ()
analyzeBlock []                                   = pure ()
analyzeBlock (Loc off (LocVarDecl vd) : rest)      = declLocal off vd (analyzeBlock rest)
analyzeBlock (Loc off (StructDeclStmt sd) : rest)  = declLocalStruct off sd (analyzeBlock rest)
analyzeBlock (Loc off s : rest)                   = withOffset off (analyzeStmt s) >> analyzeBlock rest

-- | Declare a statement-position struct (§4.4/§5): validate its field types
-- under its own offset (the same check top-level struct fields get, via
-- 'validateTopLevel'), then run @k@ with it visible in 'ctxStructs'. Same
-- "scope is 'local'" idiom as 'declLocal' — visible for exactly the rest of
-- this block, gone once @k@ returns. No redeclaration/shadow check: a struct
-- name shares cc02's separate struct namespace, not the variable scope
-- 'checkShadowRedecl' guards.
--
-- Field validation checks a struct NAME against 'ctxAllStructs' (whole
-- program, any order/scope), not the order-sensitive 'ctxStructs' — a local
-- struct field naming another struct declared later in the same block (by
-- pointer, or below in source order) is exactly as forward-reference-tolerant
-- as a top-level struct field is, per SPEC §4.4's "otherwise fully
-- forward-reference-tolerant symbol table". Only by-value CONTAINMENT
-- ordering is checked (top-level only, ERR_INCOMPLETE_STRUCT_FIELD via
-- 'C02.Analyzer.Layout') — this is a plain "does the name exist" check.
declLocalStruct :: Int -> StructDecl -> Analyze a -> Analyze a
declLocalStruct off sd k = do
  let checkFieldType = checkDeclTypeWith (\sn -> asks (Set.member sn . ctxAllStructs))
  withOffset off $ forM_ (structFields sd) $ \(bt, d, nm) -> void $ checkFieldType nm (bt, d)
  local (\c -> c { ctxStructs = Map.insert (structName sd) (structFields sd) (ctxStructs c) }) k

-- | Semantic checks for a single statement (§7). Its offset is already set by
-- 'analyzeBlock'. 'LocVarDecl' and 'StructDeclStmt' are handled by
-- 'analyzeBlock' so their bindings can scope the rest of the block; a stray
-- one here (shouldn't occur — neither is a legal @for@ init/incr clause) is a
-- no-op.
analyzeStmt :: Stmt -> Analyze ()
analyzeStmt stmt = case stmt of
  LocVarDecl vd -> do off <- asks ctxOffset; declLocal off vd (pure ())

  Nested block  -> local pushScope (analyzeBlock block)

  Assign lhs _ rhs -> do
    let ok = isLvalue lhs
    unless ok $ emit (NotLvalue "left-hand side of assignment is not assignable")
    lt <- infer lhs
    rt <- infer rhs
    when ok $ case lt of
      Just t  -> expectCompat t rt "assignment"
      Nothing -> pure ()

  Return mExpr -> do
    ret <- asks ctxReturn
    case mExpr of
      Just e  -> do actual <- infer e; expectCompat ret actual "return"
      -- a bare `return;` is only legal in a void function; otherwise it's a
      -- type mismatch of the return type against void (SPEC §7.5).
      Nothing -> when (ret /= (Void, 0)) $ emit (TypeMismatch ret (Void, 0) "return")

  If branches -> forM_ branches $ \(mCond, block) -> do
    mapM_ (void . infer) mCond
    local pushScope (analyzeBlock block)

  While cond mBody -> do
    _ <- infer cond
    local incLoop $ forM_ mBody (\b -> local pushScope (analyzeBlock b))

  -- The init declaration gets its own scope spanning cond/incr/body.
  For mInit mCond mIncr mBody -> local pushScope $
    withForInit mInit $ do
      mapM_ (void . infer) mCond
      forM_ mIncr analyzeStmt
      local incLoop $ forM_ mBody (\b -> local pushScope (analyzeBlock b))

  ExprStmt e -> void (infer e)

  Break    -> whenNotInLoop BreakOutsideLoop
  Continue -> whenNotInLoop ContinueOutsideLoop

  StructDeclStmt _ -> pure ()
  where
    whenNotInLoop d = do
      depth <- asks ctxLoopDepth
      when (depth == 0) (emit d)

-- | Run @k@ with a @for@ initializer's binding in scope, if it declares one. A
-- @for@-init has no 'Loc' of its own, so it borrows the enclosing @for@ offset.
withForInit :: Maybe Stmt -> Analyze a -> Analyze a
withForInit Nothing                k = k
withForInit (Just (LocVarDecl vd)) k = do off <- asks ctxOffset; declLocal off vd k
withForInit (Just s)               k = analyzeStmt s >> k

-- | SPEC §7.5 shallow missing-return check: only the last statement is
-- inspected, and control-flow statements are conservatively assumed to return
-- so a function ending in a returning @if/else@ isn't a false positive.
checkMissingReturn :: FuncDecl -> [Loc Stmt] -> Analyze ()
checkMissingReturn f stmts
  | (funcReturnType f, funcReturnPtrDepth f) == (Void, 0) = pure ()
  | mayReturn (lastMaybe stmts)                           = pure ()
  | otherwise                                             = emit (MissingReturn (funcName f))
  where
    lastMaybe [] = Nothing
    lastMaybe xs = Just (unLoc (last xs))

    mayReturn Nothing  = False
    mayReturn (Just s) = case s of
      Return _  -> True
      If _      -> True
      While _ _ -> True
      For{}     -> True
      Nested _  -> True
      _         -> False
