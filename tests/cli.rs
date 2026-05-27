use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

#[warn(dead_code)]
enum ExpectedOutcome {
  Success,
  Failure,
}

fn c02_binary() -> PathBuf {
  env::var("CARGO_BIN_EXE_C02")
  .map(PathBuf::from)
  .unwrap_or_else(|_| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/debug/C02"))
}

fn assert_cli_test(fixture: &str, expected: ExpectedOutcome, no_out: bool) {
  let fixture_path = Path::new(fixture);
  assert!(fixture_path.exists(), "fixture not found: {}", fixture);
  
  let mut cmd = Command::new(c02_binary());
  cmd.arg(fixture_path);
  if no_out {
    cmd.arg("--no-out");
  }
  let output = cmd.output().expect("failed to launch compiler binary");
  
  let output_path = fixture_path.with_extension("bin");
  let stdout = String::from_utf8_lossy(&output.stdout);
  let stderr = String::from_utf8_lossy(&output.stderr);
  
  match expected {
    ExpectedOutcome::Success => {
      assert!(output.status.success(),
      "{}: expected success, got failure\nstdout:\n{}\nstderr:\n{}",
      fixture_path.display(), stdout, stderr);
      if !no_out {
        assert!(output_path.exists(),
        "{}: expected output file to be created: {}",
        fixture_path.display(), output_path.display());
        fs::remove_file(&output_path).ok();
      }
    }
    ExpectedOutcome::Failure => {
      assert!(!output.status.success(),
      "{}: expected failure, got success\nstdout:\n{}\nstderr:\n{}",
      fixture_path.display(), stdout, stderr);
      if output_path.exists() {
        fs::remove_file(&output_path).ok();
      }
    }
  }
}

include!(concat!(env!("CARGO_MANIFEST_DIR"), "/tests/cli/generated_cli_tests.rs"));
