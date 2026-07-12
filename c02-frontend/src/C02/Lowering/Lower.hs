{-# OPTIONS_GHC -Wno-unused-top-binds #-}
module C02.Lowering.Lower (
  ) where
import C02.Analyzer.Types (Env (..), Ty, inferType)
import C02.Lowering.TAC (Instr(..), Operand(..), binOpToTac)
import C02.Parser.AST (Expr(..))

data LowerState = LowerState
  { nextTemp  :: Int
  , nextLabel :: Int
  }


typeOf :: Env -> Expr -> Ty
typeOf env e = case inferType env e of
  Right t -> t
  Left _  -> error "typeOf: analyzer should have caught this"


freshTemp :: LowerState -> (Int, LowerState)
freshTemp st = (nextTemp st, st { nextTemp = nextTemp st + 1 })


lowerExpr :: Env -> LowerState -> Expr -> (Operand, [Instr], LowerState)
lowerExpr env st e@(IntLit n) = (OperandConstInt (typeOf env e) n, [], st)
lowerExpr env st e@(StrLit s) = (OperandConstStr (typeOf env e) s, [], st)
lowerExpr env st e@(Var name) = (OperandVar    (typeOf env e) name, [], st)
lowerExpr env st e@(Binary op a b) =
  let (opA, instrA, st1) = lowerExpr env st a     -- lower left, using starting state
      (opB, instrB, st2) = lowerExpr env st1 b    -- lower right — which state goes in here?
      (t,   st3)         = freshTemp st2          -- burn a temp for the result
      dst   = OperandTemp (typeOf env e) t
      instr = Instr { instrOp = binOpToTac op, instrDst = dst, instrSrc1 = opA, instrSrc2 = opB, instrLabel = Nothing, instrCallName = Nothing, instrCallArgs = Nothing, instrFieldName = Nothing, instrCastType = Nothing  }
  in (dst, instrA ++ instrB ++ [instr], st3)      -- what's the full instruction list handed back?
lowerExpr env st (Unary op expr) = undefined
lowerExpr env st (Call name params) = undefined
lowerExpr env st (Cast ty depth expr) = undefined
lowerExpr env st (Deref expr) = undefined
lowerExpr env st (Field expr name) = undefined
lowerExpr env st (StructInit name initters) = undefined

