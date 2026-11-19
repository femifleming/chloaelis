@ Entry point of the benchmark image. The simulator maps RAM, enables the FPU and jumps here.
        .syntax unified
        .arm
        .section .start, "ax"
        .global _start
_start:
        ldr     sp, =__stack_top
        bl      bench_crt
        .global bench_exit
bench_exit:
        b       bench_exit
