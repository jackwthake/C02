; strcpy: Copy a null-terminated string from src to dest
;   Input:  args0 (dest), args1 (src)
;   Output: None
;   Uses:   A, X, Y
strcpy:
    ; 1. Check if src is null
    lda args1
    beq .done

    ; set src and dest pointers in Y for indexed addressing
    ldy #0
    lda args0
    sta dest_ptr
    lda args1
    sta src_ptr

    ; 2. Loop to copy characters
.loop:
    ; 3. Load byte from src and store to dest
    lda (src_ptr), y
    sta (dest_ptr), y

    ; 4. Increment pointers and check for null terminator
    inc src_ptr
    inc dest_ptr
    bne .loop

.done:
    rts