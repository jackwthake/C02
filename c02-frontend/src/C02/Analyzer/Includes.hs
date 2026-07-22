-- | Include resolution: the pre-analysis pass that consumes a freshly parsed
-- 'Program''s include list and produces an equivalent 'Program' with none. Each
-- include is replaced by the top-level declarations of the file it names, whose
-- own includes are resolved first (so includes nest). The included declarations
-- are spliced ahead of the including file's own, so a header's forward
-- declarations and struct definitions precede the code that uses them.
--
-- This is the one frontend stage that touches the filesystem to assemble a
-- translation unit, so it lives in 'IO'. It runs in the driver BETWEEN parsing
-- and the (pure) analyzer: by the time 'C02.Analyzer.Analyze.analyze' sees the
-- program its include list is empty, and 'TopLevelDecl' cannot even represent an
-- include.
module C02.Analyzer.Includes
  ( resolveIncludes
  ) where

import           Control.Exception    (IOException, try)
import           Control.Monad.Except (ExceptT, runExceptT, throwError)
import           Control.Monad.Reader (ReaderT, ask, runReaderT)
import           Control.Monad.State  (StateT, get, liftIO, modify, runStateT)
import           Data.List            (intercalate)
import           Data.List.NonEmpty   (NonEmpty ((:|)))
import           Data.Map             (Map)
import qualified Data.Map             as Map
import qualified Data.Set             as Set
import           Data.Void            (Void)
import           System.FilePath      (takeDirectory, (</>))
import System.Directory (canonicalizePath)
import           Text.Megaparsec      (ErrorFancy (ErrorFail),
                                       ParseError (FancyError),
                                       ParseErrorBundle (..), PosState (..),
                                       defaultTabWidth, initialPos)

import           C02.Parser.AST       (InclStmt (..), Loc, Program (..),
                                       TopLevelDecl, unLoc)
import           C02.Parser.Parser    (parseProgram)

-- | The resolver's working monad. From the outside in: a 'ReaderT' holding the
-- @-I@ search directories (constant for the whole resolution); a 'StateT' whose
-- map goes from each included file's path to its source text (the key set doubles
-- as the pragma-once dedup and the cycle guard, the values are retained so the
-- renderer can show source lines for diagnostics inside an included file); and an
-- error channel carrying the SAME parse-error type the driver already prints with
-- 'Text.Megaparsec.errorBundlePretty'.
type Resolve =
  ReaderT [FilePath] (StateT (Map FilePath String) (ExceptT (ParseErrorBundle String Void) IO))

-- | Resolve every include in @prog@. @path@ is the file @prog@ was parsed from;
-- an @include "name"@ is looked up first in the including file's own directory,
-- then in each of @searchDirs@ (the @-I@ directories) in order — first readable
-- match wins. Returns 'Left' if any (transitively) included file fails to parse or
-- can't be found in any search directory. On success the returned 'Program' has an
-- empty include list, paired with the source text of every file that was read (NOT
-- including @prog@'s own file, which the driver already holds) so the diagnostic
-- renderer can attribute errors to the right file.
resolveIncludes
  :: FilePath                                         -- ^ path of the file @prog@ came from
  -> [FilePath]                                       -- ^ @-I@ search directories, in order
  -> Program
  -> IO (Either (ParseErrorBundle String Void) (Program, Map FilePath String))
resolveIncludes path searchDirs prog = runExceptT $ do
  (tops, srcs) <- runStateT (runReaderT (flatten path (takeDirectory path) prog) searchDirs) Map.empty
  pure (Program [] tops, srcs)

-- | Fully resolve a parsed program to a flat list of top-levels: expand its
-- include list (recursively) and prepend the results to the program's own
-- declarations. @dir@ is the base directory for this program's relative include
-- paths.
flatten :: FilePath -> FilePath -> Program -> Resolve [Loc TopLevelDecl]
flatten self dir (Program incls tops) = do
  included <- concat <$> mapM (resolveInclude self dir) incls
  pure (included ++ tops)

-- | Resolve one @include "name"@. The candidate paths are @name@ joined onto the
-- including file's directory first, then each @-I@ search directory in order; the
-- first that reads successfully is the file.
resolveInclude :: FilePath -> FilePath -> Loc InclStmt -> Resolve [Loc TopLevelDecl]
resolveInclude self dir incl = do
  searchDirs <- ask
  let name       = includePath (unLoc incl)
      candidates = [ d </> name | d <- dir : searchDirs ]
  
  -- Check if any resolved candidate would refer to the same file as self
  selfCanonicalized <- liftIO (canonicalizePath self)
  isSelfInclude <- or <$> mapM (\path -> do
    pathCanonicalized <- liftIO (try (canonicalizePath path) :: IO (Either IOException FilePath))
    case pathCanonicalized of
      Left _  -> pure False
      Right p -> pure (p == selfCanonicalized)
    ) candidates
  case isSelfInclude of
    True  -> throwError (selfIncludeBundle name)
    False -> resolveCandidates self name candidates

-- | Try each candidate path in order. A path already visited short-circuits to
-- '[]' (this gives pragma-once dedup on diamond includes and terminates cycles —
-- @a@ includes @b@ includes @a@ finds @a@'s path already present on the second
-- visit). Otherwise the first path that reads successfully is THE file: its source
-- is recorded (marking it visited) BEFORE recursing, then it is parsed and its own
-- includes resolved. A candidate that fails to read is skipped so the next search
-- directory gets a turn; if every candidate is exhausted the include is reported
-- as not found, listing everywhere we looked.
resolveCandidates :: FilePath -> String -> [FilePath] -> Resolve [Loc TopLevelDecl]
resolveCandidates self name candidates = go candidates
  where
    go [] = throwError (notFoundBundle name candidates)
    go (path : rest) = do
      seen <- get
      if path `Map.member` seen
        then pure []
        else do
          result <- liftIO (try (readFile path) :: IO (Either IOException String))
          case result of
            Left _    -> go rest             -- not here (or unreadable): try next search dir
            Right src -> do
              modify (Map.insert path src)   -- mark visited + retain source before recursing
              case parseProgram path src of
                Left err   -> throwError err
                Right prog -> flatten self (takeDirectory path) prog

-- | A one-error bundle reporting that an @include@ couldn't be found in any search
-- directory, listing the paths that were tried. There is no source text to show,
-- so the input is empty and the error sits at offset 0: 'errorBundlePretty'
-- renders it as @name:1:1:@ followed by the message, matching the frontend's
-- parse-error style.
notFoundBundle :: String -> [FilePath] -> ParseErrorBundle String Void
notFoundBundle name candidates = ParseErrorBundle
  { bundleErrors   = FancyError 0 (Set.singleton (ErrorFail msg)) :| []
  , bundlePosState = PosState
      { pstateInput      = ""
      , pstateOffset     = 0
      , pstateSourcePos  = initialPos name
      , pstateTabWidth   = defaultTabWidth
      , pstateLinePrefix = ""
      }
  }
  where msg = "cannot find included file '" ++ name ++ "'; searched: "
              ++ intercalate ", " candidates

-- | A one-error bundle reporting that we tried to include ourselves.
-- There is no source text to show, so the input is empty and the error sits
-- at offset 0: 'errorBundlePretty' renders it as @name:1:1:@ followed by
-- the message, matching the frontend's parse-error style.
selfIncludeBundle :: String -> ParseErrorBundle String Void
selfIncludeBundle name = ParseErrorBundle
  { bundleErrors   = FancyError 0 (Set.singleton (ErrorFail msg)) :| []
  , bundlePosState = PosState
      { pstateInput      = ""
      , pstateOffset     = 0
      , pstateSourcePos  = initialPos name
      , pstateTabWidth   = defaultTabWidth
      , pstateLinePrefix = ""
      }
  }
  where msg = "cannot include yourself '" ++ name ++ "'."