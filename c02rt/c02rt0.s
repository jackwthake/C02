
_reset_handler:
    SEI             ; Disable interrupts during setup
    CLD             ; Clear decimal mode (crucial for 6502/65C02 sanity)
    LDX #$FF
    TXS             ; Initialize the hardware stack pointer to $01FF

    ; --- Clear Zero Page ---
    ; Clears completely from $00 to $FF
    LDX #$00
    LDA #$00
.clear_zp:
    STA $00,X
    INX
    BNE .clear_zp   ; Wraps from $FF back to $00 to exit cleanly
