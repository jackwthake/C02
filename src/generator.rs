use crate::parser::TopLevel;

pub fn generate(_ast: Vec<TopLevel>) -> String {
  let mut output = String::new();

  // load c02rt0.s runtime prelude
  output.push_str(include_str!("../c02rt/reg.s"));
  output.push_str(include_str!("../c02rt/c02_vectors.s"));
  output.push_str(include_str!("../c02rt/c02rt0.s"));

  let test_main = "DDRB = $6002
PORTB = $6000

_main:
    LDA #$FF
    STA DDRB
    LDA #$01
    STA PORTB";

  output.push_str(test_main);

  output
}