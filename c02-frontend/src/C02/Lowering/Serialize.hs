-- | Binary serialization of a lowered 'Module' to the on-disk IR object format
-- that @c02-as@ consumes. This is the write side of the wire contract; it must
-- match @c02-as/src/ir-serial.c@'s @read_*@ functions byte-for-byte.
--
-- Notes on the contract (why this deviates from cc02's own writer):
--
--   * Encoding is native-endian, native-width (same-machine .o reuse) — little
--     endian on the x86-64 host, so all fixed integers are @*LE@.
--   * Every field is positional and always present; an absent optional is
--     encoded as its empty form (a 0-length string, a 0 count, 'OPERAND_NONE',
--     a zeroed type), never omitted.
--   * We write exactly what @c02-as@'s reader consumes — NOT what cc02's writer
--     emits. cc02 additionally writes per-instruction asm mnemonics (a removed
--     feature) that @c02-as@'s reader does not consume, so writing them would
--     desync the stream. The per-CFG @is_interrupt@ flag, by contrast, IS part of
--     the contract as of IR version 3 (written last in each CFG, matching
--     @c02-as@'s @read_cfg@); codegen doesn't emit RTI from it yet, but the flag
--     is preserved through the object so it can.
module C02.Lowering.Serialize
  ( serializeModule
  ) where

import           Data.ByteString.Builder
import qualified Data.ByteString.Lazy as BL
import           Data.Maybe (fromMaybe)
import           Data.Word (Word32)

import           C02.Parser.AST (BaseType(..), NamedType)
import           C02.Analyzer.Types (Ty)
import           C02.Lowering.TAC


serializeModule :: Module -> BL.ByteString
serializeModule = toLazyByteString . putModule


-- ---------------------------------------------------------------------------
-- Primitives (match read_u32 / read_u64 / read_i64 / read_str)
-- ---------------------------------------------------------------------------

w32 :: Word32 -> Builder
w32 = word32LE

-- Counts, ids, offsets, depths — all unsigned 32-bit on the wire.
u32 :: Int -> Builder
u32 = word32LE . fromIntegral

-- Register addresses are unsigned 64-bit.
u64 :: Int -> Builder
u64 = word64LE . fromIntegral

-- Integer constants (operand / global int_val) are signed 64-bit.
i64 :: Int -> Builder
i64 = int64LE . fromIntegral

-- A length-prefixed string: [u32 length][bytes]. An empty/absent string is a
-- length-0 record (read_str maps length 0 to NULL). Identifiers and literals
-- are ASCII, so one byte per char.
str :: String -> Builder
str s = u32 (length s) <> string8 s

strMaybe :: Maybe String -> Builder
strMaybe = str . fromMaybe ""


-- ---------------------------------------------------------------------------
-- Types and operands
-- ---------------------------------------------------------------------------

-- read_type: kind, is_ptr, ptr_depth, then struct_name only when kind == STRUCT.
putType :: Ty -> Builder
putType (bt, depth) =
     w32 (typeKindTag bt)
  <> u32 (if depth > 0 then 1 else 0)
  <> u32 depth
  <> case bt of
       StructName nm -> str nm
       _             -> mempty

-- read_operand: kind, type (always), then the kind-specific payload.
putOperand :: Operand -> Builder
putOperand op =
     w32 (operandTag op)
  <> putType (operandType op)
  <> case op of
       OperandNone     _   -> mempty
       OperandTemp     _ i -> u32 i
       OperandVar      _ n -> str n
       OperandConstInt _ n -> i64 n
       OperandConstStr _ s -> str s


-- ---------------------------------------------------------------------------
-- Instructions and blocks
-- ---------------------------------------------------------------------------

-- read_instr order: op, dst, src1, src2, label, call_name, call_arg_count,
-- [args], field_name, cast_type. (call_name is BEFORE the arg count;
-- field_name is AFTER the args; cast_type is last.) Absent optionals flatten
-- to their empty wire form.
putInstr :: Instr -> Builder
putInstr i =
     w32 (tacOpTag (instrOp i))
  <> putOperand (instrDst i)
  <> putOperand (instrSrc1 i)
  <> putOperand (instrSrc2 i)
  <> w32 (fromMaybe 0 (instrLabel i))
  <> strMaybe (instrCallName i)
  <> u32 (length callArgs) <> foldMap putOperand callArgs
  <> strMaybe (instrFieldName i)
  <> putType (fromMaybe zeroType (instrCastType i))
  where
    callArgs = fromMaybe [] (instrCallArgs i)
    -- cast_type is only read for TAC_CAST; a zeroed u8 type is a valid filler
    -- for every other op (matches cc02's zero-initialized field).
    zeroType = (U8, 0)

-- read_block: id, instr_count, [instrs].
putBlock :: Block -> Builder
putBlock b =
     u32 (blockId b)
  <> u32 (length (instructions b))
  <> foldMap putInstr (instructions b)


-- ---------------------------------------------------------------------------
-- CFGs and module-level declarations
-- ---------------------------------------------------------------------------

putParam :: NamedType -> Builder
putParam (bt, depth, nm) = putType (bt, depth) <> str nm

-- read_cfg: name, return_type, params(count, [type, name]), blocks(count,
-- [block]), next_temp, next_label, is_interrupt. (is_interrupt is last, matching
-- cc02's write_cfg / c02-as's read_cfg.)
putCfg :: Cfg -> Builder
putCfg c =
     str (fnName c)
  <> putType (retType c)
  <> u32 (length (params c)) <> foldMap putParam (params c)
  <> u32 (length (blocks c)) <> foldMap putBlock (blocks c)
  <> u32 (nextTemp c)
  <> u32 (nextLabel c)
  <> u32 (if isInterrupt c then 1 else 0)

putField :: IRField -> Builder
putField f = str (irFieldName f) <> putType (irFieldType f) <> u32 (irFieldOffset f)

-- struct: name, field_count, total_size, then [name, type, offset] per field.
putStruct :: Struct -> Builder
putStruct s =
     str (irStructName s)
  <> u32 (length (irStructFields s))
  <> u32 (irStructSize s)
  <> foldMap putField (irStructFields s)

putGlobal :: Global -> Builder
putGlobal g =
     str (globName g)
  <> putType (globType g)
  <> w32 (irInitKindTag (globInitKind g))
  <> case globInitKind g of
       IRInitInt  -> i64 (globIntVal g)
       IRInitStr  -> str (globalStrVal g)
       IRInitNone -> mempty

putReg :: Reg -> Builder
putReg r = str (regName r) <> putType (regType r) <> u64 (regAddr r)

putExtern :: Extern -> Builder
putExtern e =
     u32 (if externIsFunc e then 1 else 0)
  <> str (externName e)
  <> putType (externType e)
  <> u32 (length (externParams e)) <> foldMap putParam (externParams e)


-- ---------------------------------------------------------------------------
-- Module — sections are emitted in the reader's order (structs, globals, regs,
-- cfgs, externs), which is NOT the record's field order.
-- ---------------------------------------------------------------------------

putModule :: Module -> Builder
putModule m =
     w32 c02Magic
  <> w32 irVersion
  <> u32 (length (structs m)) <> foldMap putStruct  (structs m)
  <> u32 (length (globals m)) <> foldMap putGlobal  (globals m)
  <> u32 (length (regs m))    <> foldMap putReg     (regs m)
  <> u32 (length (cfgs m))    <> foldMap putCfg     (cfgs m)
  <> u32 (length (externs m)) <> foldMap putExtern  (externs m)
