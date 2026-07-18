-- | Struct field-offset computation (SPEC §4.4). Pure and self-contained — no
-- 'C02.Analyzer.Analyze' 'Ctx', it re-walks the 'Program' in source order on
-- its own, because the ordering rule below needs a plain source-order list,
-- not the scope-shaped traversal pass 2 already does.
--
-- Layout is sequential with no padding or alignment (8/16-bit target), and
-- every pointer is 2 bytes regardless of pointee or depth — mirrors
-- @cc02@'s @ir-gen/ir.c@ (@collect_struct@ / @type_size@), which is the only
-- place this behaviour was previously specified.
--
-- The one real check here, @ERR_INCOMPLETE_STRUCT_FIELD@, is scoped
-- __top-level only__: SPEC §4.4 says the by-value-field ordering rule is
-- "checked by a literal position scan over top-level items", and @cc02@'s
-- @validate_toplevel_types@ literally only scans @program->program.items@.
-- A locally-declared struct (statement-position, SPEC §5) still gets a
-- layout — IR lowering needs a size for any struct type, wherever declared —
-- it just doesn't get this diagnostic; an unresolved local field silently
-- sizes as 0, the same gap @cc02@ has for locals.
module C02.Analyzer.Layout
  ( StructLayout(..)
  , computeStructLayouts
  ) where

import Data.Map (Map)
import qualified Data.Map as Map

import C02.Parser.AST
import C02.Analyzer.Diagnostic (Diagnostic(..), Diag(..))


-- | One struct's fields (source order, each paired with its byte offset) and
-- total size.
data StructLayout = StructLayout
  { layoutFields :: [(NamedType, Int)]
  , layoutSize   :: Int
  } deriving (Show, Eq)


-- | One struct declaration's position in the whole-program source order:
-- top-level ('siteTopLevel' 'True') or nested in a function body.
data StructSite = StructSite
  { siteOffset   :: Pos
  , siteTopLevel :: Bool
  , siteDecl     :: StructDecl
  }

-- | Every struct declaration in the program, in source order — top-level
-- 'StructDef's interleaved with any 'StructDeclStmt' found while walking each
-- function body (recursing into 'Nested'/'If'/'While'/'For' blocks, the only
-- statement shapes that can contain another statement; a @for@-init clause
-- can only be a var-decl or expression, never a struct decl, so it's not
-- walked here).
collectStructSites :: Program -> [StructSite]
collectStructSites (Program _ decls) = concatMap top decls
  where
    top (Loc off (StructDef s))    = [StructSite off True s]
    top (Loc _   (FunctionDecl f)) = concatMap stmt (bodyOf f)
    top _                          = []

    bodyOf f = case body f of
      Just (Nested b) -> b
      _                -> []

    stmt (Loc off (StructDeclStmt s)) = [StructSite off False s]
    stmt (Loc _   (Nested b))         = concatMap stmt b
    stmt (Loc _   (If branches))      = concatMap (concatMap stmt . snd) branches
    stmt (Loc _   (While _ mb))       = maybe [] (concatMap stmt) mb
    stmt (Loc _   (For _ _ _ mb))     = maybe [] (concatMap stmt) mb
    stmt _                            = []


-- | Byte size of a field's type, given the enclosing struct's own name (to
-- catch by-value self-reference) and the layouts of every struct already laid
-- out earlier in source order. 'Left' carries the referenced struct name that
-- couldn't be resolved (self, or not laid out yet); 'Right' the field's size.
fieldSize :: Map String StructLayout -> String -> BaseType -> Int -> Either String Int
fieldSize layouts selfName bt depth
  | depth > 0 = Right 2   -- every pointer is 2 bytes, any depth (16-bit address)
  | otherwise = case bt of
      U8   -> Right 1
      I8   -> Right 1
      U16  -> Right 2
      I16  -> Right 2
      Void -> Right 0     -- unreachable in a valid program: ERR_VOID_VARIABLE rejects this
      StructName n
        | n == selfName -> Left n
        | otherwise     -> maybe (Left n) (Right . layoutSize) (Map.lookup n layouts)

-- | Lay out one struct's fields against the layouts already computed for
-- earlier structs. Returns the layout (an unresolved field contributes 0
-- bytes, same fallback as the reference implementation) plus the referenced
-- struct name for each field that couldn't be resolved.
layOutStruct :: Map String StructLayout -> String -> [NamedType] -> (StructLayout, [String])
layOutStruct layouts selfName fields =
  (StructLayout (zip fields offsets) (sum (map fst sized)), concatMap snd sized)
  where
    sized = [ case fieldSize layouts selfName bt depth of
                Right n      -> (n, [])
                Left refName -> (0, [refName])
            | (bt, depth, _) <- fields ]
    offsets = scanl (+) 0 (map fst sized)   -- one offset per field; scanl's trailing element (total size) is dropped by zip

-- | Fold every struct declaration site into a layout map, in source order, so
-- a by-value field's pointee is always looked up in what's already been laid
-- out — exactly the declaration-order precedence SPEC §4.4 requires.
computeStructLayouts :: Program -> (Map String StructLayout, [Diag])
computeStructLayouts prog = go Map.empty [] (collectStructSites prog)
  where
    go layouts diags [] = (layouts, reverse diags)
    go layouts diags (site : rest) =
      let decl        = siteDecl site
          name         = structName decl
          (layout, bad) = layOutStruct layouts name (structFields decl)
          diags'
            | siteTopLevel site = [ At (siteOffset site) (IncompleteStructField name r) | r <- bad ] ++ diags
            | otherwise         = diags
      in go (Map.insert name layout layouts) diags' rest
