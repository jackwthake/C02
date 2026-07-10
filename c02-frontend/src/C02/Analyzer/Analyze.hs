-- | The semantic-analysis driver (SPEC §7): the walk that ties the pure type
-- relations in "C02.Analyzer.Types" to a whole 'Program'. Two conceptual passes:
--
--   * Pass 1 ('buildGlobals') registers every top-level declaration into the
--     global scope, so forward references resolve, and reports redeclarations.
--   * Pass 2 ('analyzeFunc') walks each function body, threading a scope stack,
--     the enclosing return type, and a loop depth.
--
-- Two Haskell idioms carry the design and are worth naming:
--
--   * __Accumulate, don't short-circuit.__ The pure layer returns
--     @Either Diagnostic Ty@ (first error wins); the walk runs in
--     @ReaderT Ctx (Writer [Diagnostic])@ so a failed sub-check contributes a
--     diagnostic and the walk keeps going. 'infer' bridges the two, turning a
--     'Left' into an emitted diagnostic plus a 'Nothing' "poison" — the poison
--     is just 'Maybe', so downstream checks skip an already-broken operand
--     instead of re-reporting it.
--
--   * __Scope is 'local', not push/pop.__ Entering a block runs the rest of the
--     walk under an extended 'Ctx'; returning from that sub-computation /is/ the
--     pop. A local declaration's scope is literally the recursive
--     @analyzeBlock rest@ it wraps, so nothing leaks past the block.
module C02.Analyzer.Analyze
  ( analyze
  ) where

import           Control.Monad (forM_, unless, void, when)
import           Control.Monad.Reader (ReaderT, asks, local, runReaderT)
import           Control.Monad.Writer (Writer, execWriter, tell)
import           Data.Map (Map)
import qualified Data.Map as Map
import qualified Data.Set as Set

import           C02.Parser.AST
import           C02.Analyzer.Diagnostic (Diagnostic(..))
import           C02.Analyzer.Types
                   ( Env(..), Symbol(..), Ty
                   , inferType, isTypeCompatible, isLvalue )


-- | One lexical scope frame: names visible at that nesting level.
type Scope = Map String Symbol

-- | Everything pass 2 threads through the walk. The scope list has the innermost
-- frame at its head and the global frame at its tail (never empty).
data Ctx = Ctx
  { ctxScopes    :: [Scope]                -- innermost first, global last
  , ctxStructs   :: Map String [NamedType] -- registered struct definitions
  , ctxReturn    :: Ty                     -- enclosing function's return type
  , ctxLoopDepth :: Int                    -- break/continue legal iff > 0
  }

type Analyze = ReaderT Ctx (Writer [Diagnostic])


-- | Run both passes over a program and return every diagnostic, in the SPEC's
-- emission order: pass-1 redeclarations, then top-level type validation, then
-- the missing-main check, then pass 2 over the function bodies.
analyze :: Program -> [Diagnostic]
analyze (TopLevels decls) = p1errs ++ execWriter (runReaderT walk ctx0)
  where
    (globalScope, structs, p1errs) = buildGlobals decls
    ctx0 = Ctx { ctxScopes    = [globalScope]
               , ctxStructs   = structs
               , ctxReturn    = (Void, 0)
               , ctxLoopDepth = 0 }
    walk = do
      validateTopLevel decls
      checkMain decls
      mapM_ analyzeFunc [ f | FunctionDecl f <- decls ]


-- ---------------------------------------------------------------------------
-- Small monad helpers
-- ---------------------------------------------------------------------------

emit :: Diagnostic -> Analyze ()
emit d = tell [d]

-- | Flatten the scope stack into the 'Env' the pure typer consumes. Shadowing is
-- disallowed (see 'declLocal'), so the union is conflict-free; 'Map.unions' is
-- left-biased and the innermost frame is leftmost, which is the right tie-break
-- anyway.
envOf :: Ctx -> Env
envOf c = Env (ctxStructs c) (Map.unions (ctxScopes c))

-- | Resolve an expression's type in the current context, bridging the pure
-- @Either@ typer into the accumulating walk: on an error, emit it and yield
-- 'Nothing' (poison) so callers skip the broken operand.
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
-- non-pointer @void@ is rejected (§7.4), and a struct type must name a
-- registered struct. Returns whether the type is legal; on a bad type the caller
-- still binds the name (so later uses don't re-report) but skips the init check.
checkDeclType :: String -> Ty -> Analyze Bool
checkDeclType name (bt, depth)
  | bt == Void && depth == 0 = emit (VoidVariable name) >> pure False
  | StructName sn <- bt = do
      known <- asks (Map.member sn . ctxStructs)
      if known then pure True else emit (UnknownStruct sn) >> pure False
  | otherwise = pure True

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

-- | Declare a local variable, then run the continuation with it in scope. This
-- is the "scope is 'local'" idiom: the binding is visible for exactly @k@ (the
-- rest of the block) and vanishes when @k@ returns.
declLocal :: VarDecl -> Analyze a -> Analyze a
declLocal vd k = do
  let ty   = (varType vd, ptrDepth vd)
      name = declName vd
  ok <- checkDeclType name ty
  checkShadowRedecl name
  forM_ (declInit vd) $ \e -> do
    actual <- infer e
    when ok (expectCompat ty actual name)   -- init context tag is the variable name
  local (bindLocal name (VarSym ty)) k

-- | Declare a function parameter (like 'declLocal' but no initializer).
declParam :: NamedType -> Analyze a -> Analyze a
declParam (bt, depth, name) k = do
  _ <- checkDeclType name (bt, depth)
  checkShadowRedecl name
  local (bindLocal name (VarSym (bt, depth))) k


-- ---------------------------------------------------------------------------
-- Pass 1: register globals
-- ---------------------------------------------------------------------------

-- | Fold the top-level declarations into the global scope and struct table,
-- reporting a redeclaration the second time any name appears. Structs, globals,
-- registers and functions share one namespace (SPEC §7.2), so a single "seen"
-- set guards them all — including a same-file @decl@ that duplicates a later
-- definition (SPEC S-7: @decl@ is cross-file only, never an in-file prototype).
buildGlobals :: [TopLevelDecl] -> (Scope, Map String [NamedType], [Diagnostic])
buildGlobals = go Map.empty Map.empty Set.empty []
  where
    go syms structs _    errs [] = (syms, structs, reverse errs)
    go syms structs seen errs (d : ds)
      | Set.member name seen = go syms structs seen (Redeclaration name : errs) ds
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

    funcSym f = FuncSym (funcReturnType f, funcReturnPtrDepth f)
                        [ (bt, depth) | (bt, depth, _) <- params f ]


-- | After pass 1, validate the types written in top-level declarations (now that
-- every struct name is registered, forward references resolve) and type-check
-- global initializers in the global scope.
validateTopLevel :: [TopLevelDecl] -> Analyze ()
validateTopLevel = mapM_ one
  where
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

-- | SPEC §7.6: the program must define a function named @main@ (a @decl@ forward
-- declaration alone is not a definition).
checkMain :: [TopLevelDecl] -> Analyze ()
checkMain decls =
  when (null [ () | FunctionDecl f <- decls, funcName f == "main" ]) (emit MissingMain)


-- ---------------------------------------------------------------------------
-- Pass 2: walk function bodies
-- ---------------------------------------------------------------------------

-- | Walk one function: its parameters and top-level body locals share a single
-- scope (a body local reusing a parameter name is therefore a redeclaration).
-- After the body, a non-void function whose last statement can't be seen to
-- return is flagged (§7.5, shallow).
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
funcBodyStmts :: FuncDecl -> [Stmt]
funcBodyStmts f = case body f of
  Just (Nested b) -> b
  _               -> []

-- | Walk a block, threading each local declaration into the tail via the
-- "scope is 'local'" idiom.
analyzeBlock :: [Stmt] -> Analyze ()
analyzeBlock []                     = pure ()
analyzeBlock (LocVarDecl vd : rest) = declLocal vd (analyzeBlock rest)
analyzeBlock (s : rest)             = analyzeStmt s >> analyzeBlock rest

-- | Semantic checks for a single statement (§7). 'LocVarDecl' is handled by
-- 'analyzeBlock' so its binding can scope the rest of the block; a stray one
-- here (shouldn't occur) is treated as a no-op declaration.
analyzeStmt :: Stmt -> Analyze ()
analyzeStmt stmt = case stmt of
  LocVarDecl vd -> declLocal vd (pure ())

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

  -- Struct declaration in statement position (§5): register its fields locally.
  StructDeclStmt sd ->
    local (\c -> c { ctxStructs = Map.insert (structName sd) (structFields sd) (ctxStructs c) })
          (pure ())
  where
    whenNotInLoop d = do
      depth <- asks ctxLoopDepth
      when (depth == 0) (emit d)

-- | Run @k@ with a @for@ initializer's binding in scope, if it declares one.
withForInit :: Maybe Stmt -> Analyze a -> Analyze a
withForInit Nothing               k = k
withForInit (Just (LocVarDecl vd)) k = declLocal vd k
withForInit (Just s)              k = analyzeStmt s >> k

-- | SPEC §7.5 shallow missing-return check: only the last statement is
-- inspected, and control-flow statements are conservatively assumed to return
-- so a function ending in a returning @if/else@ isn't a false positive.
checkMissingReturn :: FuncDecl -> [Stmt] -> Analyze ()
checkMissingReturn f stmts
  | (funcReturnType f, funcReturnPtrDepth f) == (Void, 0) = pure ()
  | mayReturn (lastMaybe stmts)                           = pure ()
  | otherwise                                             = emit (MissingReturn (funcName f))
  where
    lastMaybe [] = Nothing
    lastMaybe xs = Just (last xs)

    mayReturn Nothing  = False
    mayReturn (Just s) = case s of
      Return _  -> True
      If _      -> True
      While _ _ -> True
      For{}     -> True
      Nested _  -> True
      _         -> False
