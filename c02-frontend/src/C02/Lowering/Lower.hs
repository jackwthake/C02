module C02.Lowering.Lower
  ( LowerState(..)
  , lowerExpr
  , lowerStmt
  , lowerBlock
  , emptyInstr
  ) where
import C02.Analyzer.Types (Env (..), Ty, inferType, Symbol (VarSym))
import C02.Lowering.TAC (Instr(..), Operand(..), TacOp(..), binOpToTac, operandType)
import C02.Parser.AST (Expr(..), UnOp(..), BinOp(..), BaseType (..), Stmt (..), Loc (Loc), VarDecl (declName, varType, ptrDepth, declInit), AssignmentOp (..), StructDecl (structName, structFields), Block)
import qualified Data.Map as Map

data LowerState = LowerState
  { nextTemp  :: Int
  , nextLabel :: Int
  }


noOperand :: Operand
noOperand = OperandNone (Void, 0)


-- | An instruction with the given op and every other field at its empty
-- default. Build a real instruction by overriding only the fields that op
-- actually uses, e.g. @(emptyInstr TacLoad) { instrDst = t, instrSrc1 = p }@.
emptyInstr :: TacOp -> Instr
emptyInstr op = Instr
  { instrOp        = op
  , instrDst       = noOperand
  , instrSrc1      = noOperand
  , instrSrc2      = noOperand
  , instrLabel     = Nothing
  , instrCallName  = Nothing
  , instrCallArgs  = Nothing
  , instrFieldName = Nothing
  , instrCastType  = Nothing
  }


typeOf :: Env -> Expr -> Ty
typeOf env e = case inferType env e of
  Right t -> t
  Left _  -> error "typeOf: analyzer should have caught this"
 

freshTemp :: LowerState -> (Int, LowerState)
freshTemp st = (nextTemp st, st { nextTemp = nextTemp st + 1 })


-- | Hand out the next label number and bump the counter. Same shape as
-- 'freshTemp' but for the (until now unused) 'nextLabel' counter.
freshLabel :: LowerState -> (Int, LowerState)
freshLabel st = (nextLabel st, st { nextLabel = nextLabel st + 1 })


labelInstr :: Int -> Instr
labelInstr l = (emptyInstr TacLabel) { instrLabel = Just (fromIntegral l) }

jumpInstr :: Int -> Instr
jumpInstr l = (emptyInstr TacJump) { instrLabel = Just (fromIntegral l) }


-- | Emit code that jumps to @target@ when @cond@ evaluates to *false*.
-- 'TacCondJump' fires when its operand is *true* (nonzero), so we negate the
-- condition into a fresh temp and jump on the negation: original-false becomes
-- negated-true, the jump fires, and the guarded body is skipped. (This is
-- exactly cc02's if/while/for condition idiom.)
condJumpUnless :: Env -> LowerState -> Expr -> Int -> ([Instr], LowerState)
condJumpUnless env st cond target =
  let (condOp, condInstrs, st1) = lowerExpr env st cond
      (negTmp, st2)             = freshTemp st1
      negOp = OperandTemp (U8, 0) negTmp
      notI  = (emptyInstr TacNot)      { instrDst = negOp, instrSrc1 = condOp }
      jmpI  = (emptyInstr TacCondJump) { instrSrc1 = negOp, instrLabel = Just (fromIntegral target) }
  in (condInstrs ++ [notI, jmpI], st2)


-- | Byte width of a value: pointers and 16-bit types are 2, everything else 1.
tyWidth :: Ty -> Int
tyWidth (bt, depth)
  | depth > 0              = 2
  | bt == U16 || bt == I16 = 2
  | otherwise              = 1

-- | Whether a type is signed (by its base kind, ignoring pointer-ness), matching
-- cc02's @ir_is_signed@.
tySigned :: Ty -> Bool
tySigned (bt, _) = bt == I8 || bt == I16

-- | The type both operands of a (non-shift, non-pointer) binary op widen to: the
-- wider operand's type, made unsigned if either operand is unsigned (cc02's
-- @binop_common_type@ — mirrors C's usual arithmetic conversions for
-- comparisons, avoiding order-dependent signed/unsigned mismatch).
binopCommonType :: Ty -> Ty -> Ty
binopCommonType left right =
  let wider = if tyWidth left >= tyWidth right then left else right
  in if not (tySigned left) || not (tySigned right)
       then (if tyWidth wider == 2 then U16 else U8, snd wider)
       else wider

-- | Widen @op@ to @target@, emitting a 'TacCast' only if the types actually
-- differ. A constant is re-typed in place (no instruction); anything else casts
-- into a fresh temp.
widenIfNeeded :: LowerState -> Operand -> Ty -> (Operand, [Instr], LowerState)
widenIfNeeded st op target
  | operandType op == target      = (op, [], st)
  | OperandConstInt _ n <- op     = (OperandConstInt target n, [], st)
  | otherwise =
      let (t, st1) = freshTemp st
          dst  = OperandTemp target t
          cast = (emptyInstr TacCast) { instrDst = dst, instrSrc1 = op, instrCastType = Just target }
      in (dst, [cast], st1)


lowerArgs :: Env -> LowerState -> [Expr] -> ([Operand], [Instr], LowerState)
lowerArgs _   st []     = ([], [], st)                    -- no args: nothing lowered, state untouched
lowerArgs env st (e:es) =
  let (op,  instrsHead, st1) = lowerExpr env st  e         -- lower the first arg
      (ops, instrsTail, st2) = lowerArgs env st1 es        -- lower the rest
  in (op : ops, instrsHead ++ instrsTail, st2)             -- prepend this op, concat instrs in order


lowerInitters :: Env -> LowerState -> Operand -> [(String, Expr)] -> ([Instr], LowerState)
lowerInitters _   st _          []                    = ([], st)   -- no fields left
lowerInitters env st structTemp ((field, expr):rest)  =
  let (valOp, valInstrs, st1) = lowerExpr env st expr
      store = (emptyInstr TacFieldStore) { instrDst = structTemp, instrSrc1 = valOp, instrFieldName = Just field }
      (restInstrs, st2)       = lowerInitters env st1 structTemp rest
  in (valInstrs ++ [store] ++ restInstrs, st2)


lowerExpr :: Env -> LowerState -> Expr -> (Operand, [Instr], LowerState)
lowerExpr env st e@(IntLit n) = (OperandConstInt (typeOf env e) n, [], st)
lowerExpr env st e@(StrLit s) = (OperandConstStr (typeOf env e) s, [], st)
lowerExpr env st e@(Var name) = (OperandVar      (typeOf env e) name, [], st)
lowerExpr env st (Binary op a b) =
  let (opA, instrA, st1) = lowerExpr env st a     -- lower left, using starting state
      (opB, instrB, st2) = lowerExpr env st1 b    -- lower right
      -- Widen operands to a common type before the op (matching cc02's ir.c):
      -- shifts keep the left type and don't widen the count; pointer arithmetic
      -- isn't widened; everything else widens both sides, and a comparison
      -- yields u8.
      isCmp   = op `elem` [Lt, Gt, Le, Ge, Eq, Ne]
      isShift = op `elem` [Shl, Shr]
      tyA = operandType opA
      tyB = operandType opB
      (lhs, rhs, widenInstrs, resultTy, st3)
        | isShift                     = (opA, opB, [], tyA, st2)
        | snd tyA == 0 && snd tyB == 0 =
            let common        = binopCommonType tyA tyB
                (wl, wli, sl) = widenIfNeeded st2 opA common
                (wr, wri, sr) = widenIfNeeded sl  opB common
            in (wl, wr, wli ++ wri, if isCmp then (U8, 0) else common, sr)
        | otherwise                   = (opA, opB, [], tyA, st2)   -- pointer arithmetic
      (t, st4) = freshTemp st3
      dst   = OperandTemp resultTy t
      instr = (emptyInstr (binOpToTac op)) { instrDst = dst, instrSrc1 = lhs, instrSrc2 = rhs }
  in (dst, instrA ++ instrB ++ widenInstrs ++ [instr], st4)

lowerExpr env st e@(Unary op operand) = case op of
  Negate    -> valueUnary TacNeg
  Bang      -> valueUnary TacNot
  BitNot    -> valueUnary TacBnot
  AddressOf -> valueUnary TacAddrOf
  Incr      -> mutateUnary TacInc
  Decr      -> mutateUnary TacDec
  where
    valueUnary tac =
      let (opExp, instrExp, st1) = lowerExpr env st operand      -- lower operand
          (t,   st2)             = freshTemp st1                 -- burn a temp for the result
          dst                    = OperandTemp (typeOf env e) t  -- burn fresh temp
          instr = (emptyInstr tac) { instrDst = dst, instrSrc1 = opExp }
      in (dst, instrExp ++ [instr], st2)
    mutateUnary tac =
      let (opOperand, instrOperand, st1) = lowerExpr env st operand
          instr = (emptyInstr tac) { instrDst = opOperand }
      in (opOperand, instrOperand ++ [instr], st1)

lowerExpr env st e@(Call name params) =
  let (argOps, argInstrs, st1) = lowerArgs env st params
      retType = typeOf env e
      (dst, st2) = case retType of
        (Void, 0) -> (noOperand, st1)                      -- void: no temp, state passes through unchanged
        _         -> let (tmp, st') = freshTemp st1
                    in  (OperandTemp retType tmp, st')     -- non-void: temp burned, new state out
      callInstr = (emptyInstr TacCall) { instrDst = dst, instrCallName = Just name, instrCallArgs = Just argOps }
  in (dst, argInstrs ++ [callInstr], st2)

lowerExpr env st (Cast ty depth expr) =
  let (opExpr, instrExpr, st1) = lowerExpr env st expr
      (t, st2)                 = freshTemp st1
      castTy = (ty, depth)
      dst    = OperandTemp castTy t
      instr  = (emptyInstr TacCast) { instrDst = dst, instrSrc1 = opExpr, instrCastType = Just castTy }
  in (dst, instrExpr ++ [instr], st2)

lowerExpr env st e@(Deref expr) =
  let (opExp, instrExp, st1)  = lowerExpr env st expr         -- lower operand
      (t,   st2)              = freshTemp st1                 -- burn a temp for the result
      dst                     = OperandTemp (typeOf env e) t  -- burn fresh temp
      instr = (emptyInstr TacLoad) { instrDst = dst, instrSrc1 = opExp }
  in (dst, instrExp ++ [instr], st2)

lowerExpr env st e@(Field base name) =
  let (opBase, instrBase, st1) = lowerExpr env st base
      (t, st2)                 = freshTemp st1
      dst   = OperandTemp (typeOf env e) t
      instr = (emptyInstr TacFieldLoad) { instrDst = dst, instrSrc1 = opBase, instrFieldName = Just name }
  in (dst, instrBase ++ [instr], st2)

lowerExpr env st e@(StructInit _name initters) =
  let structTy           = typeOf env e             -- (StructName name, 0)
      (t, st1)           = freshTemp st
      structTemp         = OperandTemp structTy t
      (storeInstrs, st2) = lowerInitters env st1 structTemp initters
  in (structTemp, storeInstrs, st2)


-- | Lower one statement to a flat instruction stream. The @Maybe (Int, Int)@ is
-- the enclosing loop's @(continueLabel, breakLabel)@, or 'Nothing' outside any
-- loop — the explicit-threading stand-in for cc02's loop stack. It flows
-- *down*: a loop sets it for its body, and it reverts automatically because
-- inner calls get the new value while outer frames keep their own.
lowerStmt :: Env -> Maybe (Int, Int) -> LowerState -> Stmt -> ([Instr], LowerState)

lowerStmt env _loop st (Return mExpr) = case mExpr of
  Nothing -> ([bareReturn], st)                       -- `return;` — nothing to carry
  Just e  -> let (valOp, valInstrs, st1) = lowerExpr env st e
             in (valInstrs ++ [returnWith valOp], st1) -- `return e;` — value in src1
  where
    bareReturn    = (emptyInstr TacReturn) { instrSrc1 = noOperand }
    returnWith v  = (emptyInstr TacReturn) { instrSrc1 = v }

lowerStmt env _loop st (ExprStmt e) =
  let (_op, instrs, st1) = lowerExpr env st e
  in (instrs, st1)

-- A nested block is just its statements under a fresh scope; the loop context
-- flows straight through (a block doesn't start or end a loop).
lowerStmt env loop st (Nested b) = lowerBlock env loop st b

-- The right side is lowered as a value; the left side's *form* picks the write:
-- a plain copy into a variable, a store through a pointer, or a field store.
-- Compound ops (+=, -=) desugar to `lhs <op> rhs`. NOTE: that re-lowers the LHS
-- as both a value and a store target, so a side-effecting lvalue is evaluated
-- twice — only reachable via the shallow-lvalue rule (SPEC S-5, e.g. `f() += 1`)
-- and acceptable for ordinary lvalues.
lowerStmt env _loop st (Assign lhs op rhs) =
  let valueExpr = case op of
        Equals      -> rhs
        PlusEquals  -> Binary Add lhs rhs
        MinusEquals -> Binary Sub lhs rhs
        MultEquals  -> Binary Mul lhs rhs
        DivEquals   -> Binary Div lhs rhs
        ModEquals   -> Binary Mod lhs rhs
      (val, valInstrs, st1) = lowerExpr env st valueExpr
      (storeInstrs, st2) = case lhs of
        -- NOTE: a register target is indistinguishable from a variable in 'Env'
        -- (both are 'VarSym'), so it wrongly lands here as a plain COPY instead
        -- of a store to its fixed address. Registers need a name->addr map
        -- threaded through lowering (and 'lowerExpr' for reads); UNHANDLED here.
        Var name ->
          let dst = OperandVar (typeOf env lhs) name
          in ([(emptyInstr TacCopy) { instrDst = dst, instrSrc1 = val }], st1)
        Deref ptr ->
          let (ptrOp, ptrInstrs, st') = lowerExpr env st1 ptr
          in (ptrInstrs ++ [(emptyInstr TacStore) { instrDst = ptrOp, instrSrc1 = val }], st')
        Field base fname ->
          let (baseOp, baseInstrs, st') = lowerExpr env st1 base
          in (baseInstrs ++ [(emptyInstr TacFieldStore) { instrDst = baseOp, instrSrc1 = val, instrFieldName = Just fname }], st')
        _ -> error "lowerStmt Assign: non-lvalue target (analyzer should have rejected)"
  in (valInstrs ++ storeInstrs, st2)

-- if / else-if / else is a guard chain; lower it against one shared end label.
lowerStmt env loop st (If clauses) =
  let (endLabel, st1)   = freshLabel st
      (bodyInstrs, st2) = lowerClauses env loop st1 endLabel clauses
  in (bodyInstrs ++ [labelInstr endLabel], st2)

lowerStmt env _loop st (While cond mBody) =
  let (condLabel, st1)  = freshLabel st
      (endLabel,  st2)  = freshLabel st1
      (condInstrs, st3) = condJumpUnless env st2 cond endLabel
      (bodyInstrs, st4) = case mBody of
        Nothing   -> ([], st3)
        Just body -> lowerBlock env (Just (condLabel, endLabel)) st3 body
  in ( labelInstr condLabel : condInstrs ++ bodyInstrs ++ [jumpInstr condLabel, labelInstr endLabel]
     , st4 )

lowerStmt env loop st (For mInit mCond mIncr mBody) =
  let (env', initInstrs, st1) = lowerForInit env loop st mInit
      (condLabel, st2) = freshLabel st1
      (endLabel,  st3) = freshLabel st2
      (incrLabel, st4) = freshLabel st3   -- continue lands here: run incrementer, then recheck
      (condInstrs, st5) = case mCond of
        Nothing   -> ([], st4)
        Just cond -> condJumpUnless env' st4 cond endLabel
      (bodyInstrs, st6) = case mBody of
        Nothing   -> ([], st5)
        Just body -> lowerBlock env' (Just (incrLabel, endLabel)) st5 body
      (incrInstrs, st7) = case mIncr of
        Nothing   -> ([], st6)
        Just incr -> lowerStmt env' loop st6 incr
  in ( initInstrs
       ++ labelInstr condLabel : condInstrs
       ++ bodyInstrs
       ++ labelInstr incrLabel : incrInstrs
       ++ [jumpInstr condLabel, labelInstr endLabel]
     , st7 )

-- break/continue jump to the enclosing loop's labels. The analyzer already
-- rejected these outside a loop, so 'Nothing' is an unreachable safety no-op.
lowerStmt _env loop st Break = case loop of
  Just (_cont, brk) -> ([jumpInstr brk], st)
  Nothing           -> ([], st)
lowerStmt _env loop st Continue = case loop of
  Just (cont, _brk) -> ([jumpInstr cont], st)
  Nothing           -> ([], st)

-- Declarations are handled at block level (see 'lowerBlock') so they can scope
-- the rest of the block; reaching them here is an invariant violation, treated
-- as a benign no-op the way the analyzer does.
lowerStmt _env _loop st (LocVarDecl _)    = ([], st)
lowerStmt _env _loop st (StructDeclStmt _) = ([], st)


-- | Lower an if/else-if/else clause chain against a shared @end@ label. Each
-- guarded clause jumps past its body to the next clause on a false condition;
-- after a non-final body, an unconditional jump to @end@ skips the rest.
lowerClauses :: Env -> Maybe (Int, Int) -> LowerState -> Int
             -> [(Maybe Expr, Block)] -> ([Instr], LowerState)
lowerClauses _   _    st _   []                 = ([], st)
lowerClauses env loop st _   [(Nothing, block)] =           -- final `else`
  lowerBlock env loop st block                              -- no guard; falls through to end
lowerClauses env loop st end [(Just cond, block)] =         -- final guarded clause, no else
  let (condInstrs, st1) = condJumpUnless env st cond end    -- false skips straight to end
      (bodyInstrs, st2) = lowerBlock env loop st1 block
  in (condInstrs ++ bodyInstrs, st2)                        -- body falls through to end
lowerClauses env loop st end ((Just cond, block) : rest) =  -- guarded clause, more follow
  let (next, st1)       = freshLabel st
      (condInstrs, st2) = condJumpUnless env st1 cond next  -- false -> next clause
      (bodyInstrs, st3) = lowerBlock env loop st2 block
      (restInstrs, st4) = lowerClauses env loop st3 end rest
  in (condInstrs ++ bodyInstrs ++ [jumpInstr end, labelInstr next] ++ restInstrs, st4)
lowerClauses env loop st _   ((Nothing, block) : _) =       -- else before end (AST forbids); take it
  lowerBlock env loop st block


-- | Lower a @for@ initializer, which may declare a variable scoped to the
-- loop's cond/incr/body — so a declaration extends the env (and that extension
-- is returned for the caller to use across the loop), like 'lowerBlock' does.
lowerForInit :: Env -> Maybe (Int, Int) -> LowerState -> Maybe Stmt -> (Env, [Instr], LowerState)
lowerForInit env _    st Nothing                = (env, [], st)
lowerForInit env _    st (Just (LocVarDecl vd)) =
  let env'     = bindLocalVar vd env
      (i, st1) = lowerInit env st vd
  in (env', i, st1)
lowerForInit env loop st (Just s)               =
  let (i, st1) = lowerStmt env loop st s
  in (env, i, st1)


bindLocalVar :: VarDecl -> Env -> Env
bindLocalVar vd env = env { symbols = Map.insert (declName vd) (VarSym ty) (symbols env) }
  where ty = (varType vd, ptrDepth vd)


-- | Register a statement-position struct into the env for the rest of its block.
-- Emits no instructions — it only makes the struct's fields visible so a later
-- 'typeOf' on a value of that type resolves (mirrors the analyzer's
-- 'declLocalStruct').
bindLocalStruct :: StructDecl -> Env -> Env
bindLocalStruct sd env =
  env { structDefs = Map.insert (structName sd) (structFields sd) (structDefs env) }


-- | Emit the store for a local's initializer, if it has one. An uninitialized
-- local emits nothing (it exists by being referenced later); an initialized one
-- lowers its value and copies it into the variable. The value is lowered in the
-- pre-binding env (a local isn't in scope in its own initializer).
lowerInit :: Env -> LowerState -> VarDecl -> ([Instr], LowerState)
lowerInit env st vd = case declInit vd of
  Nothing -> ([], st)
  Just e  ->
    let (valOp, valInstrs, st1) = lowerExpr env st e
        ty   = (varType vd, ptrDepth vd)
        dst  = OperandVar ty (declName vd)
        copy = (emptyInstr TacCopy) { instrDst = dst, instrSrc1 = valOp }
    in (valInstrs ++ [copy], st1)


-- | Lower a block's statements in order, threading state and extending the env
-- for declarations so they scope the rest of the block (the "scope is the
-- recursive tail" idiom). The extended env never escapes the block, so nested
-- scopes are preserved. The loop context flows through untouched.
lowerBlock :: Env -> Maybe (Int, Int) -> LowerState -> [Loc Stmt] -> ([Instr], LowerState)
lowerBlock _   _    st []                             = ([], st)
lowerBlock env loop st (Loc _ (LocVarDecl vd) : rest) =
  let env'      = bindLocalVar vd env         -- vd is in scope for the REST of this block
      (i1, st1) = lowerInit env st vd         -- init lowered in the pre-binding env
      (i2, st2) = lowerBlock env' loop st1 rest
  in (i1 ++ i2, st2)
lowerBlock env loop st (Loc _ (StructDeclStmt sd) : rest) =
  let env'      = bindLocalStruct sd env      -- struct visible for the tail; emits nothing
      (i2, st1) = lowerBlock env' loop st rest
  in (i2, st1)
lowerBlock env loop st (Loc _ s : rest) =
  let (i1, st1) = lowerStmt env loop st s
      (i2, st2) = lowerBlock env loop st1 rest
  in (i1 ++ i2, st2)