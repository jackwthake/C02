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

import Data.List (isPrefixOf, partition)
import System.Environment (getArgs)
import System.Exit (exitFailure)
import System.IO (hPutStr, hPutStrLn, stderr)
import Text.Megaparsec (errorBundlePretty)
import C02.Parser.Parser (parseProgram)
import C02.Analyzer.Analyze (analyze)
import C02.Analyzer.Diagnostic (render)

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
              diags -> do
                mapM_ (hPutStrLn stderr . render) diags
                hPutStrLn stderr ""
                hPutStrLn stderr ("Semantic analysis failed with "
                                  ++ show (length diags) ++ " errors.")
                exitFailure
    _ -> hPutStrLn stderr "usage: c02-frontend [--parse-only] <file.c02>" >> exitFailure
