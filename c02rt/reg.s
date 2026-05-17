; =============================================================================
; ZERO PAGE BLUEPRINT
; =============================================================================

; Stack and Frame Trackers
SP        = $00
FP        = $02

; Virtual Compiler Registers ($1E-$1F)
r0        = $04
r1        = $06
r2        = $08
r3        = $0A
r4        = $0C
r5        = $0E
r6        = $10
r7        = $12
r8        = $14
r9        = $16
r10       = $18
r11       = $1A
r12       = $1C
r13       = $1E

; Function Argument ABI ($2E-$2F)
args0     = $20
args1     = $22
args2     = $24
args3     = $26
args4     = $28
args5     = $2A
args6     = $2C
args7     = $2E

; Memory Blitting Windows
src_ptr   = $30
dest_ptr  = $32

; System State Flags
sys_flags = $34

; User space data, up to programmer's discretion. This is where you can store global variables, string literals, etc.
usr_space = $40    ; 192 bytes reserved for user data, from $0040 to $00FF
