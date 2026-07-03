; cc_cgx.s -- minimal cybergraphics.library call stubs for the Continental Circus
; RTG presenter. The bebbo toolchain in this repo has no CGX SDK headers.

        XDEF    _BestCModeIDTagList
        XDEF    _WritePixelArray
        XREF    _CyberGfxBase

        SECTION code,CODE

_BestCModeIDTagList:
        move.l  a6,-(sp)
        move.l  _CyberGfxBase,a6
        move.l  8(sp),a0
        jsr     -60(a6)
        move.l  (sp)+,a6
        rts

_WritePixelArray:
        movem.l d2-d7/a6,-(sp)
        move.l  _CyberGfxBase,a6
        move.l  32(sp),a0
        move.l  36(sp),d0
        move.l  40(sp),d1
        move.l  44(sp),d2
        move.l  48(sp),a1
        move.l  52(sp),d3
        move.l  56(sp),d4
        move.l  60(sp),d5
        move.l  64(sp),d6
        move.l  68(sp),d7
        jsr     -126(a6)
        movem.l (sp)+,d2-d7/a6
        rts

        END
