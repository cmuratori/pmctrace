;  ========================================================================
;
;  (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
;
;  This software is provided 'as-is', without any express or implied
;  warranty. In no event will the authors be held liable for any damages
;  arising from the use of this software.
;
;  Please see https://computerenhance.com for more information
;
;  ========================================================================

global CountNonZeroesWithBranch

section .text

CountNonZeroesWithBranch:
    xor rax, rax
    xor r10, r10

.loop:
    mov r11b, [rdx + r10]
    
    cmp r11b, 0
    jz .skipsum
    inc rax
.skipsum:

    inc r10
    cmp r10, rcx
    jb .loop
    ret
