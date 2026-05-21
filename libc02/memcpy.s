; memcpy: Move n bytes from src to dest
;   Input:  args0 (dest), args1 (src), args2 (n)
;   Output: None
;   Uses:   A, X, Y
memcpy:
    ; 1. Check if n (args2) is zero
    lda args2
    beq .done

    ; use memory blitting windows for src and dest
    lda args0
    sta dest_ptr
    lda args1
    sta src_ptr

    ; 2. Loop to copy bytes
.loop:
    ; 3. Load byte from src and store to dest
    lda (src_ptr), y
    sta (dest_ptr), y

    ; 4. Increment pointers and decrement n
    inc src_ptr
    inc dest_ptr
    dec args2
    bne .loop
.done: