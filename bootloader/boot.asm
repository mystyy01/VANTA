[BITS 16]
[ORG 0x7C00]

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

; wait for keyboard input
mov ah, 0x00 ; wait for keypress function
int 0x16 ; calls keyboard service

; when keyboard input is sent, print a char for testing
mov ah, 0x0E
mov al, 'A'
int 0x10

jmp $ ; stops cpu from executing garbage memory - jumps code execution to current address (infinite loop)

times 510 - ($ - $$) db 0 ; $ - $$ current address minus start address of the section - times that until 510 to pad the bootloader -> db 0 means fill up with empty memory
dw 0xAA55 ; write the 2 byte signature so 0x55 comes first in memory