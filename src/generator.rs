use crate::parser::TopLevel;

pub fn generate(ast: Vec<TopLevel>) -> String {
  let mut output = String::new();

  // load c02rt0.s runtime prelude
  output.push_str(include_str!("../c02rt/c02rt0.s"));

  output
}