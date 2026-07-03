#!/usr/bin/env python3
"""Assemble contcirc sprite gfx + spritemap from raw ROMs, matching MAME.

sprites region (0x200000): ROM_LOAD64_WORD_SWAP of b33-06/05/04/03 at lanes 0/2/4/6.
  Each 8-byte group holds one byte-swapped 16-bit word from each of the 4 ROMs.
spritemap region (0x80000): ROM_REGION16_LE + ROM_LOAD16_WORD of b33-07 (straight copy).
Outputs sprites_asm.bin (2M) and spritemap_asm.bin (512K).
"""
import sys
RAW="../01-rom-analysis/raw/"
def rd(n): return open(RAW+n,"rb").read()

b06,b05,b04,b03 = rd("b33-06"),rd("b33-05"),rd("b33-04"),rd("b33-03")
N = len(b06)//2                      # 0x40000 words per ROM
out = bytearray(0x200000)
roms = (b06,b05,b04,b03)             # lanes 0..3 -> region byte offsets 0,2,4,6
swap = (len(sys.argv)<2 or sys.argv[1]!="noswap")
for g in range(N):
    base = g*8
    for lane,rom in enumerate(roms):
        lo,hi = rom[g*2], rom[g*2+1]
        if swap: out[base+lane*2+0],out[base+lane*2+1] = hi,lo   # WORD_SWAP
        else:    out[base+lane*2+0],out[base+lane*2+1] = lo,hi
open("sprites_asm.bin","wb").write(out)

stm = rd("b33-07.64")
open("spritemap_asm.bin","wb").write(stm)   # 16-bit LE region == raw bytes
print("wrote sprites_asm.bin (%d) spritemap_asm.bin (%d) swap=%s"%(len(out),len(stm),swap))
