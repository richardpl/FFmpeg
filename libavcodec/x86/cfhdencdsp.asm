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

factor_a: times 8 dw -1

factor_p1_n1: dw 1, -1, 1, -1, 1, -1, 1, -1,
factor_n1_p1: dw -1, 1, -1, 1, -1, 1, -1, 1,
factor_p11_n4: dw 11, -4, 11, -4, 11, -4, 11, -4,
factor_p5_p4: dw 5, 4, 5, 4, 5, 4, 5, 4,
pd_4: times 4 dd 4
pw_1: times 8 dw 1
pw_0: times 8 dw 0

SECTION .text

INIT_XMM sse2
%if ARCH_X86_64
cglobal cfhdenc_horiz_filter, 8, 12, 12, input, low, high, istride, lwidth, hwidth, width, height, x, y, temp, tt
    mov        yd, heightd
    shl  istrided, 1
    shl   lwidthd, 1
    shl   hwidthd, 1
    neg        yq
%else
cglobal cfhdenc_horiz_filter, 7, 7, 8, input, x, low, y, high, temp, width, height
    shl        xd, 1
    shl        yd, 1
    shl     tempd, 1
    shl    widthd, 1

    mov       xmp, xq
    mov       ymp, yq
    mov    tempmp, tempq

    mov        yd, r7m
    neg        yq

%define istrideq xm
%define lwidthq  ym
%define hwidthq  tempm
%endif

%if ARCH_X86_64
    mova       m8, [factor_p1_n1]
    mova       m9, [factor_n1_p1]
    mova      m10, [pw_1]
    mova      m11, [pd_4]
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

.loop:
    movu           m0, [inputq + xq * 2]
    movu           m1, [inputq + xq * 2 + 2]
    paddsw         m0, m1
    movu    [lowq+xq], m0

    movsx         ttq, word [inputq + xq * 2 - 4]
    neg           ttq
    movsx       tempq, word [inputq + xq * 2 - 2]
    neg         tempq
    add         tempq, ttq

    movsx         ttq, word [inputq + xq * 2 + 4]
    add         tempq, ttq
    movsx         ttq, word [inputq + xq * 2 + 6]
    add         tempq, ttq
    add         tempq, 4
    sar         tempq, 3
    movsx         ttq, word [inputq + xq * 2 + 0]
    add         tempq, ttq
    movsx         ttq, word [inputq + xq * 2 + 2]
    neg           ttq
    add         tempq, ttq

    movd          xm0, tempd
    packssdw       m0, m0
    pextrw      tempd, xm0, 0
    mov   word [highq+xq], tempw

    add            xq, 2
    cmp            xq, widthq
    jl .loop

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
cglobal cfhdenc_vert_filter, 8, 11, 14, input, low, high, istride, lwidth, hwidth, width, height, x, y, pos
    shl        istrided, 1
    shl         lwidthd, 1
    shl         hwidthd, 1
    shl          widthd, 1

    dec   heightd

    mova       m8, [factor_p1_n1]
    mova       m9, [factor_n1_p1]
    mova      m10, [pw_1]
    mova      m11, [pd_4]
    mova      m12, [factor_p11_n4]
    mova      m13, [factor_p5_p4]
%else
cglobal cfhdenc_vert_filter, 7, 7, 8, input, x, low, y, high, pos, width, height
    shl        xd, 1
    shl        yd, 1
    shl      posd, 1
    shl    widthd, 1

    mov       xmp, xq
    mov       ymp, yq
    mov     posmp, posq

    mov        xq, r7m
    dec        xq
    mov   widthmp, xq

%define istrideq xm
%define lwidthq  ym
%define hwidthq  posm
%define heightq  widthm

%endif

    xor        xq, xq
.loopw:
    xor        yq, yq

    mov      posq, xq
    movu       m0, [lowq + posq]
    add      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m2, m0
    punpcklwd  m0, m1
    punpckhwd  m2, m1

%if ARCH_X86_64
    pmaddwd    m0, m12
    pmaddwd    m2, m12
%else
    pmaddwd    m0, [factor_p11_n4]
    pmaddwd    m2, [factor_p11_n4]
%endif

    pxor       m4, m4
    add      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    paddd      m0, m4
    paddd      m2, m3

    paddd      m0, [pd_4]
    paddd      m2, [pd_4]

    psrad      m0, 3
    psrad      m2, 3

    mov      posq, xq
    pxor       m4, m4
    movu       m1, [highq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    paddd      m0, m4
    paddd      m2, m3

    psrad      m0, 1
    psrad      m2, 1

    packssdw   m0, m2

    movu    [inputq + posq], m0

    movu       m0, [lowq + posq]
    add      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m2, m0
    punpcklwd  m0, m1
    punpckhwd  m2, m1

%if ARCH_X86_64
    pmaddwd    m0, m13
    pmaddwd    m2, m13
%else
    pmaddwd    m0, [factor_p5_p4]
    pmaddwd    m2, [factor_p5_p4]
%endif

    pxor       m4, m4
    add      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    psubd      m0, m4
    psubd      m2, m3

    paddd      m0, [pd_4]
    paddd      m2, [pd_4]

    psrad      m0, 3
    psrad      m2, 3

    mov      posq, xq
    pxor       m4, m4
    movu       m1, [highq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    psubd      m0, m4
    psubd      m2, m3

    psrad      m0, 1
    psrad      m2, 1

    packssdw   m0, m2

    add      posq, istrideq
    movu    [inputq + posq], m0

    add        yq, 1
.looph:
    mov      posq, lwidthq
    imul     posq, yq
    sub      posq, lwidthq
    add      posq, xq

    movu       m4, [lowq + posq]

    add      posq, lwidthq
    add      posq, lwidthq
    movu       m1, [lowq + posq]

    mova       m5, m4
    punpcklwd  m4, m1
    punpckhwd  m5, m1

    mova       m6, m4
    mova       m7, m5

%if ARCH_X86_64
    pmaddwd    m4, m8
    pmaddwd    m5, m8
    pmaddwd    m6, m9
    pmaddwd    m7, m9

    paddd      m4, m11
    paddd      m5, m11
    paddd      m6, m11
    paddd      m7, m11
%else
    pmaddwd    m4, [factor_p1_n1]
    pmaddwd    m5, [factor_p1_n1]
    pmaddwd    m6, [factor_n1_p1]
    pmaddwd    m7, [factor_n1_p1]

    paddd      m4, [pd_4]
    paddd      m5, [pd_4]
    paddd      m6, [pd_4]
    paddd      m7, [pd_4]
%endif

    psrad      m4, 3
    psrad      m5, 3
    psrad      m6, 3
    psrad      m7, 3

    sub      posq, lwidthq
    movu       m0, [lowq + posq]

    mov      posq, hwidthq
    imul     posq, yq
    add      posq, xq
    movu       m1, [highq + posq]

    mova       m2, m0
    punpcklwd  m0, m1
    punpckhwd  m2, m1

    mova       m1, m0
    mova       m3, m2

%if ARCH_X86_64
    pmaddwd    m0, m10
    pmaddwd    m2, m10
    pmaddwd    m1, m8
    pmaddwd    m3, m8
%else
    pmaddwd    m0, [pw_1]
    pmaddwd    m2, [pw_1]
    pmaddwd    m1, [factor_p1_n1]
    pmaddwd    m3, [factor_p1_n1]
%endif

    paddd      m0, m4
    paddd      m2, m5
    paddd      m1, m6
    paddd      m3, m7

    psrad      m0, 1
    psrad      m2, 1
    psrad      m1, 1
    psrad      m3, 1

    packssdw   m0, m2
    packssdw   m1, m3

    mov      posq, istrideq
    imul     posq, 2
    imul     posq, yq
    add      posq, xq

    movu    [inputq + posq], m0
    add      posq, istrideq
    movu    [inputq + posq], m1

    add        yq, 1
    cmp        yq, heightq
    jl .looph

    mov      posq, lwidthq
    imul     posq, yq
    add      posq, xq
    movu       m0, [lowq + posq]
    sub      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m2, m0
    punpcklwd  m0, m1
    punpckhwd  m2, m1

%if ARCH_X86_64
    pmaddwd    m0, m13
    pmaddwd    m2, m13
%else
    pmaddwd    m0, [factor_p5_p4]
    pmaddwd    m2, [factor_p5_p4]
%endif

    pxor       m4, m4
    sub      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    psubd      m0, m4
    psubd      m2, m3

%if ARCH_X86_64
    paddd      m0, m11
    paddd      m2, m11
%else
    paddd      m0, [pd_4]
    paddd      m2, [pd_4]
%endif

    psrad      m0, 3
    psrad      m2, 3

    mov      posq, hwidthq
    imul     posq, yq
    add      posq, xq
    pxor       m4, m4
    movu       m1, [highq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    paddd      m0, m4
    paddd      m2, m3

    psrad      m0, 1
    psrad      m2, 1

    packssdw   m0, m2

    mov      posq, istrideq
    imul     posq, 2
    imul     posq, yq
    add      posq, xq
    movu    [inputq + posq], m0

    mov      posq, lwidthq
    imul     posq, yq
    add      posq, xq
    movu       m0, [lowq + posq]
    sub      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m2, m0
    punpcklwd  m0, m1
    punpckhwd  m2, m1

%if ARCH_X86_64
    pmaddwd    m0, m12
    pmaddwd    m2, m12
%else
    pmaddwd    m0, [factor_p11_n4]
    pmaddwd    m2, [factor_p11_n4]
%endif

    pxor       m4, m4
    sub      posq, lwidthq
    movu       m1, [lowq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    paddd      m0, m4
    paddd      m2, m3

%if ARCH_X86_64
    paddd      m0, m11
    paddd      m2, m11
%else
    paddd      m0, [pd_4]
    paddd      m2, [pd_4]
%endif

    psrad      m0, 3
    psrad      m2, 3

    mov      posq, hwidthq
    imul     posq, yq
    add      posq, xq
    pxor       m4, m4
    movu       m1, [highq + posq]
    mova       m3, m4
    punpcklwd  m4, m1
    punpckhwd  m3, m1

    psrad      m4, 16
    psrad      m3, 16

    psubd      m0, m4
    psubd      m2, m3

    psrad      m0, 1
    psrad      m2, 1

    packssdw   m0, m2

    mov      posq, istrideq
    imul     posq, 2
    imul     posq, yq
    add      posq, istrideq
    add      posq, xq
    movu    [inputq + posq], m0

    add        xq, mmsize
    cmp        xq, widthq
    jl .loopw
    RET
