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
import           Control.Monad.State  (StateT, get, liftIO, modify, runStateT)
import           Data.List.NonEmpty   (NonEmpty ((:|)))
import           Data.Map             (Map)
import qualified Data.Map             as Map
import qualified Data.Set             as Set
import           Data.Void            (Void)
import           System.FilePath      (takeDirectory, (</>))
import           Text.Megaparsec      (ErrorFancy (ErrorFail),
                                       ParseError (FancyError),
                                       ParseErrorBundle (..), PosState (..),
                                       defaultTabWidth, initialPos)

import           C02.Parser.AST       (InclStmt (..), Loc, Program (..),
                                       TopLevelDecl, unLoc)
import           C02.Parser.Parser    (parseProgram)

-- | The resolver's working monad: 'IO' to read files, an accumulating map from
-- each included file's path to its source text, and an error channel carrying the
-- SAME parse-error type the driver already knows how to print with
-- 'Text.Megaparsec.errorBundlePretty'. The map's key set doubles as the
-- pragma-once dedup and the cycle guard; its values are retained so the renderer
-- can show source lines for diagnostics that arise inside an included file.
type Resolve = StateT (Map FilePath String) (ExceptT (ParseErrorBundle String Void) IO)

-- | Resolve every include in @prog@. @path@ is the file @prog@ was parsed from;
-- relative include paths are resolved against its directory. Returns 'Left' if
-- any (transitively) included file fails to parse or cannot be opened. On success
-- the returned 'Program' has an empty include list, paired with the source text of
-- every file that was read (NOT including @prog@'s own file, which the driver
-- already holds) so the diagnostic renderer can attribute errors to the right
-- file.
resolveIncludes
  :: FilePath                                         -- ^ path of the file @prog@ came from
  -> Program
  -> IO (Either (ParseErrorBundle String Void) (Program, Map FilePath String))
resolveIncludes path prog = runExceptT $ do
  (tops, srcs) <- runStateT (flatten (takeDirectory path) prog) Map.empty
  pure (Program [] tops, srcs)

-- | Fully resolve a parsed program to a flat list of top-levels: expand its
-- include list (recursively) and prepend the results to the program's own
-- declarations. @dir@ is the base directory for this program's relative include
-- paths.
flatten :: FilePath -> Program -> Resolve [Loc TopLevelDecl]
flatten dir (Program incls tops) = do
  included <- concat <$> mapM (resolveInclude dir) incls
  pure (included ++ tops)

resolveInclude :: FilePath -> Loc InclStmt -> Resolve [Loc TopLevelDecl]
resolveInclude dir incl = resolveFile (dir </> includePath (unLoc incl))

-- | Read, parse, and recursively resolve one included file. The path is marked
-- visited (its source recorded in the state map) BEFORE recursing, so a cycle
-- (@a@ includes @b@ includes @a@) terminates: the second visit finds the path
-- already present and expands to nothing. The same check gives pragma-once dedup
-- on diamond includes for free.
resolveFile :: FilePath -> Resolve [Loc TopLevelDecl]
resolveFile path = do
  seen <- get
  if path `Map.member` seen
    then pure []
    else do
      -- A missing (or unreadable) header must not crash the compiler: catch the
      -- IOException and report it in the same bundle format as a parse error,
      -- attributed to the header we failed to open.
      result <- liftIO (try (readFile path) :: IO (Either IOException String))
      case result of
        Left ioErr -> throwError (missingFileBundle path ioErr)
        Right src  -> do
          modify (Map.insert path src)   -- mark visited + retain source before recursing
          case parseProgram path src of
            Left err   -> throwError err
            Right prog -> flatten (takeDirectory path) prog

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
