-- | Command-line entry point for the c02 frontend. Reads a source file, parses
-- it, and (unless @--parse-only@) runs semantic analysis. On a clean run it dumps
-- the AST to stdout; diagnostics go to stderr with a nonzero exit so the driver
-- (c02c) aborts the pipeline instead of feeding a bad program to later stages.
--
-- @--parse-only@ stops after parsing — this is what lets the parser test stage
-- exercise parsing constructs that aren't semantically valid whole programs (no
-- @main@, undeclared names, &c.) without analysis rejecting them. Later stages
-- (TAC lowering, codegen) and a @--emit symtab@ mode will be chained in here.
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
import C02.Analyzer.Analyze (analyze)
import C02.Analyzer.Diagnostic (Diag(..), Diagnostic, render)

main :: IO ()
main = do
  args <- getArgs
  let (flags, files) = partition ("--" `isPrefixOf`) args
      parseOnly      = "--parse-only" `elem` flags
  case files of
    [path] -> do
      src <- readFile path
      case parseProgram path src of
        Left err   -> hPutStr stderr (errorBundlePretty err) >> exitFailure
        Right prog
          | parseOnly -> print prog
          | otherwise -> case analyze prog of
              []    -> print prog
              diags -> hPutStr stderr (renderDiags path src diags) >> exitFailure
    _ -> hPutStrLn stderr "usage: c02-frontend [--parse-only] <file.c02>" >> exitFailure


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
