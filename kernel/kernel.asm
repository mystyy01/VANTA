[BITS 64]

mov rax, 0x0F4B0F4B0F4B0F4B   ; 4 characters: "KKKK" with color                                                                                                                         
mov [0xB8000], rax     

jmp $ ; dont execute garbage memory