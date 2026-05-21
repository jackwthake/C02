; __div16: Unsigned 16-bit division
; Inputs:  args0 (dividend), args1 (divisor)
; Output:  r0 (quotient), r1 (remainder)
; Uses:    A, X, Y (X is counter)

__div16:
    ; 1. Initialize quotient (r0) and remainder (r1)
    lda #0
    sta r0
    sta r0+1
    sta r1
    sta r1+1
    ldx #16          ; Loop 16 times for 16 bits

.loop:
    ; 2. Shift dividend into remainder (Shift Left)
    asl args0
    rol args0+1
    rol r1
    rol r1+1

    ; 3. Compare remainder with divisor
    ; Perform: r1 - args1
    sec
    lda r1
    sbc args1
    tay             ; Save low byte result in Y
    lda r1+1
    sbc args1+1

    ; 4. If remainder >= divisor, subtract and set quotient bit
    bcc .no_sub     ; If carry is clear, r1 < divisor
    sta r1+1       ; Save high byte of subtraction
    sty r1         ; Save low byte of subtraction
    sec             ; Set bit to add to quotient
    bcs .next_bit   ; Always branch

.no_sub:
    clc             ; Bit to add is 0

.next_bit:
    rol r0          ; Shift quotient left, bringing in the result bit
    rol r0+1
    dex
    bne .loop
    rts