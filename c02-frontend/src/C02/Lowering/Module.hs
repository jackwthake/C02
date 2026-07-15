-- | Module- and function-level lowering: assemble a whole 'Program' into the
-- 'T.Module' IR that 'c02-as' consumes. This is the layer above
-- "C02.Lowering.Lower" (which lowers individual expressions and statements).
--
-- TAC is imported qualified as @T@ because its record fields collide with the
-- AST's and with 'LowerState' (@params@, @regName@, @nextTemp@, …); qualifying
-- the (less-used) IR side keeps the assembly code readable.
--
-- Following cc02's @ir-gen/ir.c@: each function becomes a single-block CFG (the
-- whole body in one block, labels/jumps resolved by id from the flat stream),
-- with a trailing @RETURN@ appended if the body doesn't already end in one.
module C02.Lowering.Module
  ( lowerModule
  , lowerFunction
  , buildGlobalEnv
  ) where

import qualified Data.Map as Map

import           C02.Parser.AST
import           C02.Analyzer.Types (Env(..), Symbol(..))
import           C02.Analyzer.Layout (StructLayout(..), computeStructLayouts)
import           C02.Lowering.Lower (LowerState(..), lowerBlock, emptyInstr)
import qualified C02.Lowering.TAC as T


-- | Lower a whole program to an IR module: one CFG per defined function, plus
-- the struct layouts, globals, register definitions, and externs codegen needs.
lowerModule :: Program -> T.Module
lowerModule prog@(TopLevels decls) = T.Module
  { T.structs = map toStruct (Map.toList layouts)
  , T.globals = [ toGlobal v | Loc _ (GlobalVarDecl v) <- decls ]
  , T.regs    = [ toReg r    | Loc _ (RegisterDecl r)  <- decls ]
  , T.cfgs    = [ lowerFunction genv f | Loc _ (FunctionDecl f) <- decls ]
  , T.externs = [ toExtern d | Loc _ d <- decls, isExtern d ]
  }
  where
    genv          = buildGlobalEnv decls
    (layouts, _)  = computeStructLayouts prog


-- | The environment every function body starts from: struct definitions plus
-- the global symbol table (globals, registers, functions, and forward decls),
-- mirroring the analyzer's pass-1 registration.
buildGlobalEnv :: [Loc TopLevelDecl] -> Env
buildGlobalEnv decls = Env
  { structDefs = Map.fromList [ (structName s, structFields s) | Loc _ (StructDef s) <- decls ]
  , symbols    = Map.fromList (concatMap sym decls)
  , registers  = Map.fromList [ (regName r, declAddress r) | Loc _ (RegisterDecl r) <- decls ]
  }
  where
    sym (Loc _ d) = case d of
      GlobalVarDecl v -> [(declName v, VarSym (varType v, ptrDepth v))]
      FwdVarDecl v    -> [(declName v, VarSym (varType v, ptrDepth v))]
      RegisterDecl r  -> [(regName r,  VarSym (regType r, regPtrDepth r))]
      FunctionDecl f  -> [(funcName f, funcSym f)]
      FwdFuncDecl f   -> [(funcName f, funcSym f)]
      StructDef _     -> []
    funcSym f = FuncSym (funcReturnType f, funcReturnPtrDepth f)
                        [ (bt, d) | (bt, d, _) <- params f ]


-- | Lower one function to a single-block CFG. Parameters are added to the
-- global env for the body; the counters that survive lowering become the CFG's
-- @next_temp@ / @next_label@ (codegen trusts them, so they must be right).
lowerFunction :: Env -> FuncDecl -> T.Cfg
lowerFunction genv f = T.Cfg
  { T.fnName      = funcName f
  , T.retType     = (funcReturnType f, funcReturnPtrDepth f)
  , T.params      = params f
  , T.blocks      = [ T.Block { T.blockId = 0, T.instructions = allInstrs, T.successors = [] } ]
  , T.nextTemp    = nextTemp finalSt
  , T.nextLabel   = nextLabel finalSt
  , T.isInterrupt = isInterrupt f
  }
  where
    fenv      = genv { symbols = foldr addParam (symbols genv) (params f) }
    addParam (bt, d, nm) = Map.insert nm (VarSym (bt, d))
    bodyStmts = case body f of
      Just (Nested b) -> b
      _               -> []
    (bodyInstrs, finalSt) = lowerBlock fenv Nothing (LowerState 0 0) bodyStmts
    -- Codegen expects every function to end in a RETURN; add one if the body's
    -- last statement wasn't already a return (§7.5 lets a void function fall off
    -- the end).
    allInstrs
      | endsInReturn = bodyInstrs
      | otherwise    = bodyInstrs ++ [emptyInstr T.TacReturn]
    endsInReturn = case reverse bodyInstrs of
      (i : _) -> T.instrOp i == T.TacReturn
      []      -> False


-- | One laid-out struct becomes an IR struct: field name, type, and byte offset,
-- plus total size (all from 'computeStructLayouts').
toStruct :: (String, StructLayout) -> T.Struct
toStruct (name, layout) = T.Struct
  { T.irStructName   = name
  , T.irStructFields = [ T.IRField { T.irFieldName = fname, T.irFieldType = (bt, d), T.irFieldOffset = off }
                       | ((bt, d, fname), off) <- layoutFields layout ]
  , T.irStructSize   = layoutSize layout
  }


-- | A global variable and its constant initializer. Only literal initializers
-- are representable in the wire format; a negated literal is folded here, and
-- anything else lowers as uninitialized (a limitation, not silently wrong data).
toGlobal :: VarDecl -> T.Global
toGlobal v = T.Global
  { T.globName     = declName v
  , T.globType     = (varType v, ptrDepth v)
  , T.globInitKind = kind
  , T.globIntVal   = ival
  , T.globalStrVal = sval
  }
  where
    (kind, ival, sval) = case declInit v of
      Just (IntLit n)                -> (T.IRInitInt, n,       "")
      Just (Unary Negate (IntLit n)) -> (T.IRInitInt, negate n, "")
      Just (StrLit s)                -> (T.IRInitStr, 0,       s)
      _                              -> (T.IRInitNone, 0,      "")


toReg :: RegDecl -> T.Reg
toReg r = T.Reg
  { T.regName = regName r
  , T.regType = (regType r, regPtrDepth r)
  , T.regAddr = declAddress r
  }


isExtern :: TopLevelDecl -> Bool
isExtern (FwdFuncDecl _) = True
isExtern (FwdVarDecl _)  = True
isExtern _               = False

-- | A forward declaration becomes an extern symbol (resolved at link time).
toExtern :: TopLevelDecl -> T.Extern
toExtern (FwdFuncDecl f) = T.Extern
  { T.externIsFunc = True
  , T.externName   = funcName f
  , T.externType   = (funcReturnType f, funcReturnPtrDepth f)
  , T.externParams = params f
  }
toExtern (FwdVarDecl v) = T.Extern
  { T.externIsFunc = False
  , T.externName   = declName v
  , T.externType   = (varType v, ptrDepth v)
  , T.externParams = []
  }
toExtern _ = error "toExtern: not a forward declaration"
