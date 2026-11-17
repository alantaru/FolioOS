/* FolioOS — ponte BIOS: binário plano ligado em 0x10000 (stub.asm salta para o primeiro byte) */
ENTRY(bridge_main)
SECTIONS {
    . = 0x10000;
    .text : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { __bss_start = .; *(.bss .bss.* COMMON) __bss_end = .; }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(__bss_end < 0x68000, "a ponte BIOS passou do espaço abaixo da pilha provisória (0x70000)");
