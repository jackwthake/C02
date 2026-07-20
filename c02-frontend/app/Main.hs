-- | Command-line entry point for the c02 frontend. Reads a source file, parses
-- it, and (unless @--parse-only@) runs semantic analysis. On a clean run it lowers
-- and serializes the IR to an object file (@-o@, default @a.o@); diagnostics go to
-- stderr with a nonzero exit so the driver (c02c) aborts the pipeline instead of
-- feeding a bad program to later stages.
--
-- @--parse-only@ stops after parsing — this is what lets the parser test stage
-- exercise parsing constructs that aren't semantically valid whole programs (no
-- @main@, undeclared names, &c.) without analysis rejecting them.
--
-- @--dump-ast@ still runs full analysis but, on a clean run, prints the AST to
-- stdout instead of emitting the IR object. This is the analyzer test stage's
-- observable: a text stdout it can diff against a golden, rather than a binary
-- @.o@. Later stages (TAC lowering, codegen) and a @--emit symtab@ mode will be
-- chained in here.
module Main (main) where

import Data.List (isPrefixOf, partition, sortOn, stripPrefix)
import qualified Data.List.NonEmpty as NE
import Data.Map (Map)
import qualified Data.Map as Map
import qualified Data.Set as Set
import Data.Void (Void)
import System.Environment (getArgs)
import System.Exit (exitFailure)
import System.IO (hPutStr, hPutStrLn, stderr)
import Text.Megaparsec
  ( errorBundlePretty
  , ParseErrorBundle(..), ParseError(FancyError), ErrorFancy(ErrorFail)
  , PosState(..), initialPos, defaultTabWidth )
import C02.Parser.Parser (parseProgram)
import C02.Analyzer.Includes (resolveIncludes)
import C02.Analyzer.Analyze (analyze)
import C02.Analyzer.Diagnostic (Diag(..), Diagnostic, Severity(..), render, severityOf, diagWhat)
import C02.Lowering.Module (lowerModule)
import C02.Parser.AST (Program, Pos(..))
import C02.Lowering.Serialize (serializeModule)

import qualified Data.ByteString.Lazy as B

writeOutput :: Program -> String -> IO ()
writeOutput p fp =
  let m = lowerModule p
      bytes = serializeModule m
  in B.writeFile fp bytes


main :: IO ()
main = do
  args <- getArgs
  let (flags, rest0)      = partition ("--" `isPrefixOf`) args
      parseOnly           = "--parse-only" `elem` flags
      dumpAST             = "--dump-ast" `elem` flags
      (includeDirs, rest) = extractIncludeDirs rest0
      (outPath, files)    = extractOutput rest
  case files of
    [path] -> do
      src <- readFile path
      case parseProgram path src of
        Left err   -> hPutStr stderr (errorBundlePretty err) >> exitFailure
        Right prog
          -- --dump-ast is an orthogonal reporting flag: it prints the AST to
          -- stdout at whatever point we stop, so both test stages can diff a
          -- text golden instead of a binary .o. --parse-only stops after parsing
          -- (parser stage: no analysis, since its fixtures aren't whole programs);
          -- otherwise analysis runs and a clean pass emits the IR object.
          | parseOnly -> if dumpAST then print prog else pure ()
          | otherwise -> do
              resolved <- resolveIncludes path includeDirs prog
              case resolved of
                Left err                    -> hPutStr stderr (errorBundlePretty err) >> exitFailure
                Right (resolvedProg, srcs)  ->
                  -- The root file's source isn't in @srcs@ (the resolver only
                  -- records files it read); add it so the renderer can show root
                  -- diagnostics too.
                  let diags  = analyze resolvedProg
                      hasErr = any ((== Error) . severityOf . diagWhat) diags
                  in do
                    if null diags
                      then pure ()
                      else hPutStr stderr (renderDiags path (Map.insert path src srcs) diags)
                    if hasErr
                      then exitFailure
                      else if dumpAST then print resolvedProg else writeOutput resolvedProg outPath
    _ -> hPutStrLn stderr "usage: c02-frontend [--parse-only] [-I <dir>]... [-o <out.o>] <file.c02>" >> exitFailure


-- | Pull an optional @-o <path>@ out of the arguments, defaulting to @a.o@.
extractOutput :: [String] -> (FilePath, [String])
extractOutput args = case break (== "-o") args of
  (before, "-o" : path : after) -> (path, before ++ after)
  _                             -> ("a.o", args)


-- | Pull every @-I@ header search directory out of the arguments, preserving
-- order (search precedence). Both the joined form @-Idir@ and the separated form
-- @-I dir@ are accepted, matching the C compiler convention; a dangling @-I@ with
-- no following directory is ignored. Returns the directories and the remaining
-- arguments.
extractIncludeDirs :: [String] -> ([FilePath], [String])
extractIncludeDirs = go [] []
  where
    go dirs rest [] = (reverse dirs, reverse rest)
    go dirs rest (a : as)
      | a == "-I" = case as of
          (d : as') -> go (d : dirs) rest as'   -- "-I" "dir"
          []        -> go dirs rest as          -- dangling "-I": ignore
      | Just d <- stripPrefix "-I" a, not (null d) = go (d : dirs) rest as  -- "-Idir"
      | otherwise = go dirs (a : rest) as


-- | Render analyzer diagnostics in the frontend's parser-error style. A
-- megaparsec 'ParseErrorBundle' renders offsets against a SINGLE source string,
-- walking it forward and unable to rewind — so a bundle is inherently one file's
-- worth of diagnostics. After include resolution splices several files together,
-- located diagnostics ('At') are therefore grouped BY FILE, and each file's group
-- becomes its own bundle (sorted by ascending offset) rendered against that file's
-- source from @srcs@. 'errorBundlePretty' prints each with a @file:line:col@
-- header, the source line, and a caret — identical to a parse error. Free
-- diagnostics (whole-unit, no span) print as plain lines attributed to the root
-- file, then a summary count closes the report.
renderDiags :: FilePath -> Map FilePath String -> [Diag] -> String
renderDiags rootPath srcs diags =
  concatMap fileBundle (Map.toList byFile) ++ freeStr ++ summary
  where
    nErrors   = length [ () | d <- diags, severityOf (diagWhat d) == Error ]
    nWarnings = length diags - nErrors
    summary
      -- Preserve the original wording exactly when there are no warnings
      -- (every existing golden pins this string, always-plural "errors").
      | nErrors > 0 && nWarnings == 0 = "\nSemantic analysis failed with " ++ show nErrors ++ " errors.\n"
      | nErrors > 0                   = "\nSemantic analysis failed with " ++ show nErrors
                                         ++ " errors and " ++ show nWarnings ++ " warnings.\n"
      | otherwise                     = "\n" ++ show nWarnings ++ " warnings.\n"
    frees  = [ d | Free d <- diags ]
    -- group located diags by file; Map.toList then yields files in a stable
    -- (path-sorted) order, and each group is non-empty by construction.
    byFile = Map.fromListWith (++) [ (f, [(off, d)]) | At (Pos f off) d <- diags ]

    fileBundle (file, offs) = case Map.lookup file srcs of
      -- Source retained: render a proper caret bundle against it.
      Just src -> errorBundlePretty (mkBundle file src (sortOn fst offs))
      -- No source on hand (shouldn't happen): fall back to plain message lines
      -- rather than crash on a partial 'PosState'.
      Nothing  -> concat [ file ++ ": " ++ render d ++ "\n" | (_, d) <- sortOn fst offs ]

    freeStr = concat [ rootPath ++ ": " ++ render d ++ "\n" | d <- frees ]

    mkBundle :: FilePath -> String -> [(Int, Diagnostic)] -> ParseErrorBundle String Void
    mkBundle file src offs = ParseErrorBundle
      { bundleErrors   = NE.map toErr (NE.fromList offs)  -- offs is non-empty (see byFile)
      , bundlePosState = PosState
          { pstateInput      = src
          , pstateOffset     = 0
          , pstateSourcePos  = initialPos file
          , pstateTabWidth   = defaultTabWidth
          , pstateLinePrefix = ""
          }
      }
    toErr (off, d) = FancyError off (Set.singleton (ErrorFail (render d)))
