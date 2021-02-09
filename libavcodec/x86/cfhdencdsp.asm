;******************************************************************************
;* x86-optimized functions for the CFHD encoder
;* Copyright (c) 2021 Paul B Mahol
;*
;* This file is part of FFmpeg.
;*
;* FFmpeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* FFmpeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION_RODATA

pw_p1_n1:  dw  1, -1, 1, -1, 1, -1, 1, -1
pw_n1_p1:  dw  -1, 1, -1, 1, -1, 1, -1, 1
pw_p5_n11: dw  5, -11, 5, -11, 5, -11, 5, -11
pw_p11_n5: dw 11, -5, 11, -5, 11, -5, 11, -5
pd_4:  times 4 dd  4
pw_p4: times 8 dw  4
pw_n4: times 8 dw -4
pw_p1: times 8 dw  1
pw_n1: times 8 dw -1

SECTION .text

INIT_XMM sse2
%if ARCH_X86_64
cglobal cfhdenc_horiz_filter, 8, 11, 12, input, low, high, istride, lwidth, hwidth, width, height, x, y, temp
    mov        yd, heightd
    shl  istrided, 1
    shl   lwidthd, 1
    shl   hwidthd, 1
    neg        yq
%else
cglobal cfhdenc_horiz_filter, 7, 7, 8, input, x, low, y, high, temp, width, height
TODO
%endif
.looph:
    movsx          xq, word [inputq]

    movsx       tempq, word [inputq + 2]
    add         tempq, xq

    movd          xm0, tempd
    packssdw       m0, m0
    pextrw      tempd, xm0, 0
    mov   word [lowq], tempw

    movsx          xq, word [inputq]
    imul           xq, 5
    movsx       tempq, word [inputq + 2]
    imul        tempq, -11
    add         tempq, xq

    movsx          xq, word [inputq + 4]
    imul           xq, 4
    add         tempq, xq

    movsx          xq, word [inputq + 6]
    imul           xq, 4
    add         tempq, xq

    movsx          xq, word [inputq + 8]
    imul           xq, -1
    add         tempq, xq

    movsx          xq, word [inputq + 10]
    imul           xq, -1
    add         tempq, xq

    add         tempq, 4
    sar         tempq, 3

    movd          xm0, tempd
    packssdw       m0, m0
    pextrw      tempd, xm0, 0
    mov  word [highq], tempw

    mov            xq, 2

.loopw:
    movu           m0, [inputq + xq * 2]
    movu           m1, [inputq + xq * 2 + mmsize]

    pmaddwd        m0, [pw_p1]
    pmaddwd        m1, [pw_p1]

    packssdw       m0, m1
    movu    [lowq+xq], m0

    movu           m2, [inputq + xq * 2 - 4]
    movu           m3, [inputq + xq * 2 - 4 + mmsize]

    pmaddwd        m2, [pw_n1]
    pmaddwd        m3, [pw_n1]

    movu           m0, [inputq + xq * 2 + 4]
    movu           m1, [inputq + xq * 2 + 4 + mmsize]

    pmaddwd        m0, [pw_p1]
    pmaddwd        m1, [pw_p1]

    paddd          m0, m2
    paddd          m1, m3

    paddd          m0, [pd_4]
    paddd          m1, [pd_4]

    psrad          m0, 3
    psrad          m1, 3

    movu           m5, [inputq + xq * 2 + 0]
    movu           m6, [inputq + xq * 2 + mmsize]

    pmaddwd        m5, [pw_p1_n1]
    pmaddwd        m6, [pw_p1_n1]

    paddd          m0, m5
    paddd          m1, m6

    packssdw       m0, m1
    movu   [highq+xq], m0

    add            xq, mmsize
    cmp            xq, widthq
    jl .loopw

    add          lowq, widthq
    add         highq, widthq
    add        inputq, widthq
    add        inputq, widthq

    movsx          xq, word [inputq - 4]
    movsx       tempq, word [inputq - 2]
    add         tempq, xq

    movd          xm0, tempd
    packssdw       m0, m0
    pextrw      tempd, xm0, 0
    mov word [lowq-2], tempw

    movsx       tempq, word [inputq - 4]
    imul        tempq, 11
    movsx          xq, word [inputq - 2]
    imul           xq, -5
    add         tempq, xq

    movsx          xq, word [inputq - 6]
    imul           xq, -4
    add         tempq, xq

    movsx          xq, word [inputq - 8]
    imul           xq, -4
    add         tempq, xq

    movsx          xq, word [inputq - 10]
    add         tempq, xq

    movsx          xq, word [inputq - 12]
    add         tempq, xq

    add         tempq, 4
    sar         tempq, 3

    movd          xm0, tempd
    packssdw       m0, m0
    pextrw      tempd, xm0, 0
    mov word [highq-2], tempw

    sub        inputq, widthq
    sub        inputq, widthq
    sub         highq, widthq
    sub          lowq, widthq

    add          lowq, lwidthq
    add         highq, hwidthq
    add        inputq, istrideq
    add            yq, 1
    jl .looph

    RET

INIT_XMM sse2
%if ARCH_X86_64
cglobal cfhdenc_vert_filter, 8, 11, 8, input, low, high, istride, lwidth, hwidth, width, height, x, y, pos
    shl  istrided, 1
    shl    widthd, 1
%else
cglobal cfhdenc_vert_filter, 7, 7, 8, input, low, high, istride, lwitdh
TODO
%endif

    sub   heightq, 2

    xor        xq, xq
.loopw:
    mov        yq, 2

    mov      posq, xq
    movu       m0, [inputq + posq]
    add      posq, istrideq
    movu       m1, [inputq + posq]

    paddsw     m0, m1

    movu    [lowq + xq], m0

    mov      posq, xq

    movu       m0, [inputq + posq]
    add      posq, istrideq
    movu       m1, [inputq + posq]
    add      posq, istrideq
    movu       m2, [inputq + posq]
    add      posq, istrideq
    movu       m3, [inputq + posq]
    add      posq, istrideq
    movu       m4, [inputq + posq]
    add      posq, istrideq
    movu       m5, [inputq + posq]

    mova       m6, m0
    punpcklwd  m0, m1
    punpckhwd  m1, m6

    mova       m6, m2
    punpcklwd  m2, m3
    punpckhwd  m3, m6

    mova       m6, m4
    punpcklwd  m4, m5
    punpckhwd  m5, m6

    pmaddwd    m0, [pw_p5_n11]
    pmaddwd    m1, [pw_p5_n11]
    pmaddwd    m2, [pw_p4]
    pmaddwd    m3, [pw_p4]
    pmaddwd    m4, [pw_n1]
    pmaddwd    m5, [pw_n1]

    paddd      m0, m2
    paddd      m1, m3
    paddd      m0, m4
    paddd      m1, m5

    paddd      m0, [pd_4]
    paddd      m1, [pd_4]

    psrad      m0, 3
    psrad      m1, 3
    packssdw   m0, m1

    movu   [highq + xq], m0

.looph:

    mov      posq, istrideq
    imul     posq, yq
    add      posq, xq

    movu       m0, [inputq + posq]

    add      posq, istrideq
    movu       m1, [inputq + posq]

    paddsw     m0, m1

    mov      posq, lwidthq
    imul     posq, yq
    add      posq, xq

    movu    [lowq + posq], m0

    add        yq, -2

    mov      posq, istrideq
    imul     posq, yq
    add      posq, xq

    movu       m0, [inputq + posq]
    add      posq, istrideq
    movu       m1, [inputq + posq]
    add      posq, istrideq
    movu       m2, [inputq + posq]
    add      posq, istrideq
    movu       m3, [inputq + posq]
    add      posq, istrideq
    movu       m4, [inputq + posq]
    add      posq, istrideq
    movu       m5, [inputq + posq]

    add        yq, 2

    mova       m6, m0
    punpcklwd  m0, m1
    punpckhwd  m1, m6

    mova       m6, m2
    punpcklwd  m2, m3
    punpckhwd  m3, m6

    mova       m6, m4
    punpcklwd  m4, m5
    punpckhwd  m5, m6

    pmaddwd    m0, [pw_n1]
    pmaddwd    m1, [pw_n1]
    pmaddwd    m2, [pw_p1_n1]
    pmaddwd    m3, [pw_n1_p1]
    pmaddwd    m4, [pw_p1]
    pmaddwd    m5, [pw_p1]

    paddd      m0, m4
    paddd      m1, m5

    paddd      m0, [pd_4]
    paddd      m1, [pd_4]

    psrad      m0, 3
    psrad      m1, 3
    paddd      m0, m2
    paddd      m1, m3
    packssdw   m0, m1

    mov      posq, hwidthq
    imul     posq, yq
    add      posq, xq

    movu   [highq + posq], m0

    add        yq, 2
    cmp        yq, heightq
    jl .looph

    mov      posq, istrideq
    imul     posq, yq
    add      posq, xq

    movu       m0, [inputq + posq]
    add      posq, istrideq
    movu       m1, [inputq + posq]

    paddsw     m0, m1

    mov      posq, lwidthq
    imul     posq, yq
    add      posq, xq

    movu    [lowq + posq], m0

    sub        yq, 4

    mov      posq, istrideq
    imul     posq, yq
    add      posq, xq

    movu       m0, [inputq + posq]
    add      posq, istrideq
    movu       m1, [inputq + posq]
    add      posq, istrideq
    movu       m2, [inputq + posq]
    add      posq, istrideq
    movu       m3, [inputq + posq]
    add      posq, istrideq
    movu       m4, [inputq + posq]
    add      posq, istrideq
    movu       m5, [inputq + posq]

    add        yq, 4

    mova       m6, m0
    punpcklwd  m0, m1
    punpckhwd  m1, m6

    mova       m6, m2
    punpcklwd  m2, m3
    punpckhwd  m3, m6

    mova       m6, m4
    punpcklwd  m4, m5
    punpckhwd  m5, m6

    pmaddwd    m0, [pw_p1]
    pmaddwd    m1, [pw_p1]
    pmaddwd    m2, [pw_n4]
    pmaddwd    m3, [pw_n4]
    pmaddwd    m4, [pw_p11_n5]
    pmaddwd    m5, [pw_p11_n5]

    paddd      m0, m2
    paddd      m1, m3

    paddd      m0, m4
    paddd      m1, m5

    paddd      m0, [pd_4]
    paddd      m1, [pd_4]

    psrad      m0, 3
    psrad      m1, 3
    packssdw   m0, m1

    mov      posq, hwidthq
    imul     posq, yq
    add      posq, xq

    movu   [highq + posq], m0

    add        xq, mmsize
    cmp        xq, widthq
    jl .loopw
    RET
