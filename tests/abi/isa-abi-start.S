.section .text.start,"ax",@progbits
.globl _start
.type _start, @function
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    andi sp, sp, -16
    call bench_main
    li a7, 93
    ecall
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
