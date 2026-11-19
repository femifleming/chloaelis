/* One flat RAM region at the AM335x DDR base. */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 96M }
SECTIONS
{
  .text : { __text_start = .; KEEP(*(.start)) *(.text*) *(.gnu.linkonce.t.*) KEEP(*(.init)) KEEP(*(.fini)) *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx) . = ALIGN(64); __text_end = .; } > RAM
  .rodata : { *(.rodata*) *(.gnu.linkonce.r.*) } > RAM
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > RAM
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx* .gnu.linkonce.armexidx.*) __exidx_end = .; } > RAM
  .eh_frame : { KEEP(*(.eh_frame*)) } > RAM
  .init_array :
  {
    __preinit_array_start = .; KEEP(*(.preinit_array)) __preinit_array_end = .;
    __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) KEEP(*(.ctors)) __init_array_end = .;
    __fini_array_start = .; KEEP(*(.fini_array*)) KEEP(*(.dtors)) __fini_array_end = .;
  } > RAM
  .data : ALIGN(64) { *(.data*) *(.gnu.linkonce.d.*) } > RAM
  .bss : ALIGN(64) { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(64); __bss_end__ = .; } > RAM
  . = ALIGN(4096);
  __heap_start = .;
  __stack_top = ORIGIN(RAM) + LENGTH(RAM) - 64;
  /* the top third is the firmware's "big heap" (sample and delay memory, od::BigHeap) */
  __bigheap_end = __stack_top - 0x100000;
  __bigheap_start = __bigheap_end - 0x2000000;
  __heap_end = __bigheap_start;
  end = __heap_start;
}
