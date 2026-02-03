[BITS 16]
[ORG 0x7C00]
mov [boot_drive], dl
; V
mov ah, 0x0E ; moves the function call to print a letter to high bits of AX
mov al, 'V' ; moves the char 'V' into the low bits of AX 
int 0x10 ; uses BIOS interrupt to display graphics to the screen - looks for AX -> finds basically print('V')

; A
mov al, 'A'
int 0x10

; N
mov al, 'N'
int 0x10

; T
mov al, 'T'
int 0x10

; A
mov al, 'A'
int 0x10


mov ax, 0x0000
mov es, ax ; now es = 0, cant set it directly

mov ah, 0x02
mov al, 0x80 ; read 128 sectors (64KB)
mov ch, 0x00
mov cl, 0x02
mov dh, 0x00
mov dl, [boot_drive] ; use the bios passed boot drive
mov bx, 0x7E00 ; next sector after boot loader
int 0x13 ; load kernel
; --- Clear page tables ---
mov edi, 0x1000
mov ecx, 0x0C00
xor eax, eax
rep stosd

; --- Set up page table entries ---
mov dword [0x1000], 0x2003
mov dword [0x2000], 0x3003
mov dword [0x3000], 0x0083

; --- Load CR3 ---
mov eax, 0x1000
mov cr3, eax

; --- Enable PAE ---
mov eax, cr4
or eax, 1 << 5
mov cr4, eax

; --- Enable Long Mode ---
mov ecx, 0xC0000080
rdmsr
or eax, 1 << 8
wrmsr

; --- Enable Paging + Protected Mode ---
mov eax, cr0
or eax, (1 << 31) | (1 << 0)
mov cr0, eax

; --- Now go to 64-bit ---
cli                                                                                                                                                                                     
lgdt [gdt_descriptor]                                                                                                                                                                   
jmp 0x08:long_mode_start  
[BITS 64]
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, 0x90000
    jmp 0x7E00
gdt_start:                                                                                                                                                                              
    dq 0x0000000000000000          ; null

gdt_code:                                                                                                                                                                               
    dw 0xFFFF                       ; limit
    dw 0x0000                       ; base low
    db 0x00                         ; base mid
    db 10011010b                    ; access: present, ring 0, executable, readable
    db 10101111b                    ; flags: 4KB granularity, LONG MODE, limit high
    db 0x00                         ; base high
                                                                                                                                                                                        
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                    ; access: present, ring 0, writable
    db 11001111b                    ; flags: 4KB granularity, 32-bit (ignored in long mode for data)
    db 0x00

gdt_user_data:                      ; offset 0x18, selector 0x1B with RPL 3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b                    ; access: present, ring 3, writable
    db 11001111b                    ; flags: 4KB granularity
    db 0x00

gdt_user_code:                      ; offset 0x20, selector 0x23 with RPL 3
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b                    ; access: present, ring 3, executable, readable
    db 10101111b                    ; flags: 4KB granularity, LONG MODE
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start
boot_drive: db 0


times 510 - ($ - $$) db 0 ; $ - $$ current address minus start address of the section - times that until 510 to pad the bootloader -> db 0 means fill up with empty memory
dw 0xAA55 ; write the 2 byte signature so 0x55 comes first in memory
