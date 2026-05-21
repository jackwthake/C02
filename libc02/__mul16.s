; __mul16:
;   Input:  args0 (multiplicand), args1 (multiplier)
;   Output: r0 (result)
;   Uses:   A, X, Y

__mul16:
    ; 1. Initialize result (r0) to 0
    lda #0
    sta r0
    sta r0+1
    
    ; 2. Store multiplicand (args0) in a temp location
    lda args0
    sta temp_m
    lda args0+1
    sta temp_m+1
    
    ldx #16         ; Loop 16 times
.loop:
    ; 3. Shift multiplier (args1) right to check the bit
    lsr args1+1
    ror args1
    bcc .no_add     ; If bit is 0, skip addition

    ; 4. Add multiplicand to the result
    clc
    lda r0
    adc temp_m
    sta r0
    lda r0+1
    adc temp_m+1
    sta r0+1

.no_add:
    ; 5. Shift multiplicand left for the next bit position
    asl temp_m
    rol temp_m+1
    
    dex
    bne .loop
    rts

; NOTE: Requires 2 bytes of storage for the temp multiplicand
temp_m: .byte 0, 0