; ============================================================================
; PIEG - kernel/entry.asm
;
; Multiboot2 entry (GRUB, BIOS or UEFI) -> 32-bit protected mode -> long mode
; -> kernel_main(uint32_t magic, uint32_t mbi)   [rdi = magic, rsi = mbi]
;
; Build:  nasm -f elf64 kernel/entry.asm -o build/entry.o
; Link:   ld -m elf_x86_64 -T linker.ld ...   (kernel linked at 1 MiB,
;         .multiboot must be the FIRST input section in linker.ld)
;
; Debug output goes to port 0xE9 (QEMU: -debugcon stdio):
;   E = entry reached        M = bad multiboot2 magic
;   L = CPU has no long mode K = about to call kernel_main
;   X = CPU exception before kernel installed its own IDT
; ============================================================================

MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0
MB2_BOOT_MAGIC  equ 0x36D76289

%macro DBG 1
    mov dx, 0xE9
    mov al, %1
    out dx, al
%endmacro

; ----------------------------------------------------------------------------
; Multiboot2 header
; ----------------------------------------------------------------------------
section .multiboot
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd header_end - header_start
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (header_end - header_start))

    ; Framebuffer request. flags=1 -> optional. 0x0x32 -> "use whatever the
    ; firmware/GRUB picks, prefer 32 bpp". Real width/height/pitch/bpp come
    ; back in the boot info (tag type 8). Do NOT force a resolution: laptop
    ; panels often don't offer 1024x768.
    align 8
    dw 5            ; type: framebuffer
    dw 1            ; flags: optional
    dd 20           ; size
    dd 0            ; width  (any)
    dd 0            ; height (any)
    dd 32           ; depth

    ; End tag.
    align 8
    dw 0
    dw 0
    dd 8
header_end:

; ----------------------------------------------------------------------------
; 32-bit entry
; ----------------------------------------------------------------------------
section .text
BITS 32

global _start
extern kernel_main

_start:
    cli
    cld

    ; Save Multiboot2 registers FIRST (cpuid below clobbers ebx).
    mov [boot_magic], eax
    mov [boot_info], ebx

    mov esp, stack_top

    DBG 'E'

    ; Must have been loaded by a Multiboot2 bootloader.
    cmp dword [boot_magic], MB2_BOOT_MAGIC
    jne .bad_magic

    ; Long mode available?
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; Load GDT.
    lgdt [gdt_pointer]

    ; Zero the 6 page-table pages (PML4, PDPT, 4x PD) = 6 * 1024 dwords.
    mov edi, pml4_table
    xor eax, eax
    mov ecx, (6 * 4096) / 4
    rep stosd

    ; PML4[0] -> PDPT
    mov eax, pdpt_table
    or  eax, 0x03                   ; present | writable
    mov [pml4_table], eax

    ; PDPT[0..3] -> four page directories (covers 0 .. 4 GiB)
    mov edi, pdpt_table
    mov eax, pd_table
    or  eax, 0x03
    mov ecx, 4
.pdpt_loop:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x1000
    add edi, 8
    loop .pdpt_loop

    ; 2048 x 2 MiB pages, identity mapped: 0 .. 4 GiB.
    ; This covers the UEFI GOP framebuffer, which almost always lives
    ; above 1 GiB (typically 0x80000000..0xE0000000) on real laptops.
    mov edi, pd_table
    mov eax, 0x83                   ; present | writable | 2 MiB page
    mov ecx, 512 * 4
.pd_loop:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .pd_loop

    ; Enable PAE.
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; Load PML4.
    mov eax, pml4_table
    mov cr3, eax

    ; Enable long mode (EFER.LME).
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; Enable paging.
    mov eax, cr0
    or  eax, 1 << 31
    mov cr0, eax

    ; Far jump into the 64-bit code segment.
    jmp 0x08:long_mode_start

.bad_magic:
    DBG 'M'
    jmp .halt32

.no_long_mode:
    DBG 'L'

.halt32:
    cli
    hlt
    jmp .halt32

; ----------------------------------------------------------------------------
; 64-bit entry
; ----------------------------------------------------------------------------
BITS 64

long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax

    ; Stack (16-byte aligned).
    mov esp, stack_top
    and rsp, -16
    xor ebp, ebp

    ; Enable SSE so compiler-generated SSE code can't #UD.
    mov rax, cr0
    and ax, 0xFFFB                  ; clear EM
    or  ax, 0x0002                  ; set MP
    mov cr0, rax
    mov rax, cr4
    or  ax, (3 << 9)                ; OSFXSR | OSXMMEXCPT
    mov cr4, rax

    ; Early IDT: every vector -> early_fault_handler, so an early fault
    ; prints 'X' on port 0xE9 and halts instead of silently triple-faulting.
    ; The kernel replaces this with its own IDT later.
    mov eax, early_fault_handler    ; zero-extends into rax
    mov edi, idt_table
    mov ecx, 256
.idt_fill:
    mov word  [rdi + 0], ax         ; offset 15:0
    mov word  [rdi + 2], 0x08       ; code selector
    mov byte  [rdi + 4], 0          ; IST
    mov byte  [rdi + 5], 0x8E       ; present, DPL0, interrupt gate
    mov rdx, rax
    shr rdx, 16
    mov word  [rdi + 6], dx         ; offset 31:16
    mov rdx, rax
    shr rdx, 32
    mov dword [rdi + 8], edx        ; offset 63:32
    mov dword [rdi + 12], 0
    add rdi, 16
    dec ecx
    jnz .idt_fill
    lidt [idt_pointer]

    ; kernel_main(uint32_t magic, uint32_t mbi): rdi, rsi.
    mov edi, [boot_magic]
    mov esi, [boot_info]

    DBG 'K'

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

early_fault_handler:
    DBG 'X'
.halt:
    cli
    hlt
    jmp .halt

; ----------------------------------------------------------------------------
; Read-only data
; ----------------------------------------------------------------------------
section .rodata
align 8

gdt:
    dq 0x0000000000000000           ; null
    dq 0x00AF9A000000FFFF           ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF           ; 0x10: data
gdt_end:

gdt_pointer:
    dw gdt_end - gdt - 1
    dd gdt

align 8
idt_pointer:
    dw 256 * 16 - 1
    dq idt_table

; ----------------------------------------------------------------------------
; BSS
; ----------------------------------------------------------------------------
section .bss
align 4096

; These four must stay contiguous and in this order (zeroed as one block).
pml4_table:     resb 4096
pdpt_table:     resb 4096
pd_table:       resb 4096 * 4

align 16
idt_table:      resb 256 * 16

align 16
stack_bottom:   resb 65536
stack_top:

align 8
boot_magic:     resd 1
boot_info:      resd 1

section .note.GNU-stack noalloc noexec nowrite progbits
