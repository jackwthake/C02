
_reset_handler:
    SEI             ; Disable interrupts during setup
    CLD             ; Clear decimal mode (crucial for 6502/65C02 sanity)
    LDX #$FF
    TXS             ; Initialize the hardware stack pointer to $01FF
