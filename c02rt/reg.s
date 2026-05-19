; =============================================================================
; ZERO PAGE BLUEPRINT
; =============================================================================

; Stack and Frame Trackers
SP        = $00

; Virtual Compiler Registers ($02-$1E)
r0        = $02
r1        = $04
r2        = $06
r3        = $08
r4        = $0A
r5        = $0C
r6        = $0E
r7        = $10
r8        = $12
r9        = $14
r10       = $16
r11       = $18
r12       = $1A
r13       = $1C
r14       = $1E

; Function Argument ABI ($20-$2E)
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
