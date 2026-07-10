-- | Command-line entry point for the c02 frontend: read a source file, run the
-- parser stage, and print either the AST or a pretty parse error. Later stages
-- (semantic analysis, TAC lowering) will be chained in here.
module Main (main) where

import System.Environment (getArgs)
import System.Exit (exitFailure)
import System.IO (hPutStr, hPutStrLn, stderr)
import Text.Megaparsec (errorBundlePretty)
import C02.Parser.Parser (parseProgram)

main :: IO ()
main = do
  args <- getArgs
  case args of
    [path] -> do
      src <- readFile path
      case parseProgram path src of
        -- Diagnostics go to stderr and set a failure exit code, so the driver
        -- (c02c) aborts the pipeline instead of feeding a half-parsed program
        -- to later stages. The AST dump stays on stdout for a clean parse.
        Left err   -> hPutStr stderr (errorBundlePretty err) >> exitFailure
        Right prog -> print prog
    _ -> hPutStrLn stderr "usage: c02-frontend <file.c02>" >> exitFailure
