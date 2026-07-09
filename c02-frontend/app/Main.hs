-- | Command-line entry point for the c02 frontend: read a source file, run the
-- parser stage, and print either the AST or a pretty parse error. Later stages
-- (semantic analysis, TAC lowering) will be chained in here.
module Main (main) where

import System.Environment (getArgs)
import Text.Megaparsec (errorBundlePretty)
import C02.Parser (parseProgram)

main :: IO ()
main = do
  args <- getArgs
  case args of
    [path] -> do
      src <- readFile path
      case parseProgram path src of
        Left err   -> putStr (errorBundlePretty err)
        Right prog -> print prog
    _ -> putStrLn "usage: c02-frontend <file.c02>"
