; =============================================================================
; RESET VECTORS
; =============================================================================
  .org $fffa
  .word $0000 ; NMI handler (not used, but must be defined)
  .word _reset_handler ; Reset handler
  .word $0000 ; IRQ handler (not used, but must be defined)
