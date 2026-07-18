-- | Include resolution: the pre-analysis pass that turns a freshly parsed
-- 'Program' still containing @include@ statements into an equivalent one that
-- has none. Each 'IncludeStmt' is replaced in place by the top-level
-- declarations of the file it names — which are themselves resolved first, so
-- includes nest.
--
-- This is the one frontend stage that touches the filesystem to assemble a
-- translation unit, so it lives in 'IO'. It runs in the driver BETWEEN parsing
-- and the (pure) analyzer: by the time 'C02.Analyzer.Analyze.analyze' sees the
-- program, no 'IncludeStmt' nodes remain and it need not know includes exist.
module C02.Analyzer.Includes
  ( resolveIncludes
  ) where

import           Control.Exception    (IOException, try)
import           Control.Monad.Except (ExceptT, runExceptT, throwError)
import           Control.Monad.State  (StateT, evalStateT, get, liftIO, modify)
import           Data.List.NonEmpty   (NonEmpty ((:|)))
import           Data.Set             (Set)
import qualified Data.Set             as Set
import           Data.Void            (Void)
import           System.FilePath      (takeDirectory, (</>))
import           Text.Megaparsec      (ErrorFancy (ErrorFail),
                                       ParseError (FancyError),
                                       ParseErrorBundle (..), PosState (..),
                                       defaultTabWidth, initialPos)

import           C02.Parser.AST       (InclStmt (..), Loc, Program (..),
                                       TopLevelDecl (..), unLoc)
import           C02.Parser.Parser    (parseProgram)

-- | The resolver's working monad: 'IO' to read files, an accumulating set of
-- already-included paths (both the pragma-once dedup and the cycle guard), and
-- an error channel carrying the SAME parse-error type the driver already knows
-- how to print with 'Text.Megaparsec.errorBundlePretty'.
type Resolve = StateT (Set FilePath) (ExceptT (ParseErrorBundle String Void) IO)

-- | Resolve every include in @prog@. @path@ is the file @prog@ was parsed from;
-- relative include paths are resolved against its directory. Returns 'Left' if
-- any (transitively) included file fails to parse.
resolveIncludes
  :: FilePath                                         -- ^ path of the file @prog@ came from
  -> Program
  -> IO (Either (ParseErrorBundle String Void) Program)
resolveIncludes path (TopLevels decls) =
  runExceptT (evalStateT (TopLevels <$> resolveTops (takeDirectory path) decls) Set.empty)

-- | Resolve a list of top-levels: ordinary declarations pass through untouched;
-- each include expands to the (already-resolved) declarations of the file it
-- names. @dir@ is the base directory for this list's relative include paths.
resolveTops :: FilePath -> [Loc TopLevelDecl] -> Resolve [Loc TopLevelDecl]
resolveTops dir decls = concat <$> mapM (resolveDecl dir) decls

resolveDecl :: FilePath -> Loc TopLevelDecl -> Resolve [Loc TopLevelDecl]
resolveDecl dir decl = case unLoc decl of
  IncludeStmt incl -> resolveFile (dir </> includePath incl)
  _                -> pure [decl]

-- | Read, parse, and recursively resolve one included file. The path is marked
-- visited BEFORE recursing, so a cycle (@a@ includes @b@ includes @a@)
-- terminates: the second visit finds the path already present and expands to
-- nothing. The same check gives pragma-once dedup on diamond includes for free.
resolveFile :: FilePath -> Resolve [Loc TopLevelDecl]
resolveFile path = do
  seen <- get
  if path `Set.member` seen
    then pure []
    else do
      modify (Set.insert path)
      -- A missing (or unreadable) header must not crash the compiler: catch the
      -- IOException and report it in the same bundle format as a parse error,
      -- attributed to the header we failed to open.
      result <- liftIO (try (readFile path) :: IO (Either IOException String))
      case result of
        Left ioErr -> throwError (missingFileBundle path ioErr)
        Right src  -> case parseProgram path src of
          Left err                -> throwError err
          Right (TopLevels decls) -> resolveTops (takeDirectory path) decls

-- | A one-error bundle reporting that @path@ could not be opened. There is no
-- source text to show, so the input is empty and the error sits at offset 0:
-- 'errorBundlePretty' renders it as @path:1:1:@ followed by the message,
-- matching the frontend's parse-error style.
missingFileBundle :: FilePath -> IOException -> ParseErrorBundle String Void
missingFileBundle path ioErr = ParseErrorBundle
  { bundleErrors   = FancyError 0 (Set.singleton (ErrorFail msg)) :| []
  , bundlePosState = PosState
      { pstateInput      = ""
      , pstateOffset     = 0
      , pstateSourcePos  = initialPos path
      , pstateTabWidth   = defaultTabWidth
      , pstateLinePrefix = ""
      }
  }
  where msg = "cannot open included file: " ++ show ioErr
