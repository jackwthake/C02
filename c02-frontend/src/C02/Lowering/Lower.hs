{-# OPTIONS_GHC -Wno-unused-top-binds #-}
module C02.Lowering.Lower (
  ) where
import C02.Analyzer.Types (Env (..), Ty, inferType)
import C02.Lowering.TAC (Instr(..), Operand(..), TacOp(..), binOpToTac)
import C02.Parser.AST (Expr(..), UnOp(..), BaseType (Void))

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
lowerExpr env st e@(Binary op a b) =
  let (opA, instrA, st1) = lowerExpr env st a     -- lower left, using starting state
      (opB, instrB, st2) = lowerExpr env st1 b    -- lower right — which state goes in here?
      (t,   st3)         = freshTemp st2          -- burn a temp for the result
      dst   = OperandTemp (typeOf env e) t
      instr = (emptyInstr (binOpToTac op)) { instrDst = dst, instrSrc1 = opA, instrSrc2 = opB }
  in (dst, instrA ++ instrB ++ [instr], st3)      -- add new instructions in order

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
