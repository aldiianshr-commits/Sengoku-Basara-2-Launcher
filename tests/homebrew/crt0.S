    .set noreorder
    .section .text.start
    .globl _start
_start:
    lui   $sp, 0x0180          # stack at 24 MB
    jal   main
    nop
    move  $a0, $v0
    li    $v1, 4               # Exit syscall
    syscall
1:  b 1b
    nop
