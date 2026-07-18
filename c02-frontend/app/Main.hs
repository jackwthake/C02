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

import Data.List (isPrefixOf, partition, sortOn)
import qualified Data.List.NonEmpty as NE
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
import C02.Analyzer.Diagnostic (Diag(..), Diagnostic, render)
import C02.Lowering.Module (lowerModule)
import C02.Parser.AST (Program)
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
  let (flags, rest)    = partition ("--" `isPrefixOf`) args
      parseOnly        = "--parse-only" `elem` flags
      dumpAST          = "--dump-ast" `elem` flags
      (outPath, files) = extractOutput rest
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
              resolved <- resolveIncludes path prog
              case resolved of
                Left err           -> hPutStr stderr (errorBundlePretty err) >> exitFailure
                Right resolvedProg -> case analyze resolvedProg of
                  []    -> if dumpAST then print resolvedProg else writeOutput resolvedProg outPath
                  diags -> hPutStr stderr (renderDiags path src diags) >> exitFailure
    _ -> hPutStrLn stderr "usage: c02-frontend [--parse-only] [-o <out.o>] <file.c02>" >> exitFailure


-- | Pull an optional @-o <path>@ out of the arguments, defaulting to @a.o@.
extractOutput :: [String] -> (FilePath, [String])
extractOutput args = case break (== "-o") args of
  (before, "-o" : path : after) -> (path, before ++ after)
  _                             -> ("a.o", args)


-- | Render analyzer diagnostics in the frontend's parser-error style. Located
-- diagnostics ('At') are collected into one megaparsec 'ParseErrorBundle', so
-- 'errorBundlePretty' prints each with a @file:line:col@ header, the source line,
-- and a caret — identical to a parse error. That renderer walks the input forward
-- and can't rewind, so the located diagnostics MUST be sorted by ascending offset
-- first. Free diagnostics (whole-unit, no span) print as plain lines, then a
-- summary count closes the report.
renderDiags :: FilePath -> String -> [Diag] -> String
renderDiags path src diags =
  bundleStr ++ freeStr ++ "\nSemantic analysis failed with " ++ show n ++ " errors.\n"
  where
    n         = length diags
    located   = sortOn fst [ (off, d) | At off d <- diags ]
    frees     = [ d | Free d <- diags ]
    bundleStr = case NE.nonEmpty located of
      Nothing -> ""
      Just ne -> errorBundlePretty (mkBundle ne)
    freeStr   = concat [ path ++ ": " ++ render d ++ "\n" | d <- frees ]

    mkBundle :: NE.NonEmpty (Int, Diagnostic) -> ParseErrorBundle String Void
    mkBundle ne = ParseErrorBundle
      { bundleErrors   = NE.map toErr ne
      , bundlePosState = PosState
          { pstateInput      = src
          , pstateOffset     = 0
          , pstateSourcePos  = initialPos path
          , pstateTabWidth   = defaultTabWidth
          , pstateLinePrefix = ""
          }
      }
    toErr (off, d) = FancyError off (Set.singleton (ErrorFail (render d)))
