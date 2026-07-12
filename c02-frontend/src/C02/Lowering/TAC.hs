module C02.Lowering.TAC (
  c02Magic,
  irVersion,
  TacOp(..),
  IRInitKind(..),
  Operand (..),
  Instr (..),
  Block (..),
  Cfg (..),
  Global (..),
  Struct (..),
  Reg (..),
  Extern (..),
  Module (..),
  typeKindTag,
  tacOpTag,
  operandTag,
  operandType,
  irInitKindTag,
  binOpToTac
  ) where

import C02.Parser.AST (BaseType(..), NamedType, BinOp (..) )
import C02.Analyzer.Types
import Data.Word (Word32)

c02Magic :: Word32
c02Magic = 0x43303249 -- "C02I"

irVersion :: Word32
irVersion = 2

-- This MUST match the order in c02-as/src/ir.h order
data TacOp = TacAdd       | TacSub    | TacMul      | TacDiv  | TacMod
           | TacLt        | TacGt     | TacLte      | TacGte  | TacEq | TacNeq
           | TacAnd       | TacOr
           | TacShl       | TacShr    | TacBand     | TacBxor | TacBor
           | TacNeg       | TacNot    | TacBnot
           | TacInc       | TacDec
           | TacAddrOf    | TacLoad   | TacStore
           | TacCopy
           | TacLabel     | TacJump   | TacCondJump
           | TacCall      | TacReturn | TacContinue | TacBreak
           | TacFieldLoad | TacFieldStore
           | TacCast
  deriving (Show, Eq)


data IRInitKind = IRInitNone | IRInitInt | IRInitStr
  deriving (Show, Eq)


-- Each variant carries the Ty every wire operand has, plus whatever payload
-- that operand kind needs (see 'operandTag' for the matching wire numbers,
-- and 'operandType' to pull the common Ty back out).
data Operand
  = OperandNone     Ty
  | OperandTemp     Ty Int
  | OperandVar      Ty String
  | OperandConstInt Ty Int
  | OperandConstStr Ty String
  deriving (Show, Eq)


data Instr = Instr
  { instrOp        :: TacOp
  , instrDst       :: Operand
  , instrSrc1      :: Operand
  , instrSrc2      :: Operand
  , instrLabel     :: Maybe Word32
  , instrCallName  :: Maybe String
  , instrCallArgs  :: Maybe [Operand]
  , instrFieldName :: Maybe String
  , instrCastType  :: Maybe Ty
  }


data Block = Block
  { blockId      :: Int
  , instructions :: [Instr]
  , successors   :: [Block]
  }


data Cfg = Cfg
  { fnName    :: String
  , retType   :: Ty
  , params    :: [NamedType]
  , blocks    :: [Block]
  , nextTemp  :: Int
  , nextLabel :: Int
  }


data Global = Global
  { globName     :: String
  , globType     :: Ty
  , globInitKind :: IRInitKind
  , globIntVal   :: Int
  , globalStrVal :: String
  }


data Struct = Struct
  { structName :: String
  , structFields :: [NamedType]
  }


data Reg = Reg
  { regName :: String
  , regType :: Ty
  , regAddr :: Int
  }


data Extern = Extern
  { externIsFunc :: Bool
  , externName   :: String
  , externType   :: Ty
  , externParams :: [NamedType]
  }


data Module = Module
  { externs :: [Extern] 
  , structs :: [Struct]
  , globals :: [Global]
  , regs    :: [Reg]
  , cfgs    :: [Cfg]
  }


typeKindTag :: BaseType -> Word32
typeKindTag U8             = 0 -- TYPE_U8
typeKindTag I8             = 1 -- TYPE_I8
typeKindTag U16            = 2 -- TYPE_U16
typeKindTag I16            = 3 -- TYPE_I16
typeKindTag Void           = 4 -- TYPE_VOID
typeKindTag (StructName _) = 5 -- TYPE_STRUCT


tacOpTag :: TacOp -> Word32
tacOpTag TacAdd        = 0  -- TAC_ADD
tacOpTag TacSub        = 1  -- TAC_SUB
tacOpTag TacMul        = 2  -- TAC_MUL
tacOpTag TacDiv        = 3  -- TAC_DIV
tacOpTag TacMod        = 4  -- TAC_MOD
tacOpTag TacLt         = 5  -- TAC_LT
tacOpTag TacGt         = 6  -- TAC_GT
tacOpTag TacLte        = 7  -- TAC_LTE
tacOpTag TacGte        = 8  -- TAC_GTE
tacOpTag TacEq         = 9  -- TAC_EQ
tacOpTag TacNeq        = 10 -- TAC_NEQ
tacOpTag TacAnd        = 11 -- TAC_AND
tacOpTag TacOr         = 12 -- TAC_OR
tacOpTag TacShl        = 13 -- TAC_SHL
tacOpTag TacShr        = 14 -- TAC_SHR
tacOpTag TacBand       = 15 -- TAC_BAND
tacOpTag TacBxor       = 16 -- TAC_BXOR
tacOpTag TacBor        = 17 -- TAC_BOR
tacOpTag TacNeg        = 18 -- TAC_NEG
tacOpTag TacNot        = 19 -- TAC_NOT
tacOpTag TacBnot       = 20 -- TAC_BNOT
tacOpTag TacInc        = 21 -- TAC_INC
tacOpTag TacDec        = 22 -- TAC_DEC
tacOpTag TacAddrOf     = 23 -- TAC_ADDR_OF
tacOpTag TacLoad       = 24 -- TAC_LOAD
tacOpTag TacStore      = 25 -- TAC_STORE
tacOpTag TacCopy       = 26 -- TAC_COPY
tacOpTag TacLabel      = 27 -- TAC_LABEL
tacOpTag TacJump       = 28 -- TAC_JUMP
tacOpTag TacCondJump   = 29 -- TAC_COND_JUMP
tacOpTag TacCall       = 30 -- TAC_CALL
tacOpTag TacReturn     = 31 -- TAC_RETURN
tacOpTag TacContinue   = 32 -- TAC_CONTINUE
tacOpTag TacBreak      = 33 -- TAC_BREAK
tacOpTag TacFieldLoad  = 34 -- TAC_FIELD_LOAD
tacOpTag TacFieldStore = 35 -- TAC_FIELD_STORE
tacOpTag TacCast       = 36 -- TAC_CAST


operandTag :: Operand -> Word32
operandTag (OperandNone     _ )   = 0 -- OPERAND_NONE
operandTag (OperandTemp     _ _)  = 1 -- OPERAND_TEMP
operandTag (OperandVar      _ _)  = 2 -- OPERAND_VAR
operandTag (OperandConstInt _ _)  = 3 -- OPERAND_CONST_INT
operandTag (OperandConstStr _ _)  = 4 -- OPERAND_CONST_STR


-- | Every 'Operand' variant carries a 'Ty' first; pull it out regardless of
-- which kind this operand is.
operandType :: Operand -> Ty
operandType (OperandNone     t)   = t
operandType (OperandTemp     t _) = t
operandType (OperandVar      t _) = t
operandType (OperandConstInt t _) = t
operandType (OperandConstStr t _) = t


irInitKindTag :: IRInitKind -> Word32
irInitKindTag IRInitNone = 0 -- IR_INIT_NONE
irInitKindTag IRInitInt  = 1 -- IR_INIT_INT
irInitKindTag IRInitStr  = 2 -- IR_INIT_STR


binOpToTac :: BinOp -> TacOp
binOpToTac Add    = TacAdd
binOpToTac Sub    = TacSub
binOpToTac Mul    = TacMul
binOpToTac Div    = TacDiv
binOpToTac Mod    = TacMod
binOpToTac Shl    = TacShl
binOpToTac Shr    = TacShr
binOpToTac Lt     = TacLt
binOpToTac Gt     = TacGt
binOpToTac Le     = TacLte
binOpToTac Ge     = TacGte
binOpToTac Eq     = TacEq
binOpToTac Ne     = TacNeq
binOpToTac BitAnd = TacBand
binOpToTac BitXor = TacBxor
binOpToTac BitOr  = TacBor
binOpToTac And    = TacAnd
binOpToTac Or     = TacOr

