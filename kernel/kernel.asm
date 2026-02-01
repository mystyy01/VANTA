[BITS 16]

mov ah, 0x0E
mov al, 'K' ; k for kernel to show it loaded
int 0x10

jmp $ ; dont execute garbage memory