# S.C.I. / Special Criminal Investigation

Initial Taito Z scaffold for the 1989 S.C.I. parent set (`sci.zip`).

This port is forked from the Chase HQ native shell because S.C.I. is the direct
successor and shares the dual-68000 + TC0100SCN + TC0150ROD + YM2610 shape. The
S.C.I.-specific work already split out here:

- CRC-checked ROM extraction from `/home/jon/Downloads/sci.zip`
- S.C.I. main/sub memory maps
- 0x20000 sound CPU ROM and YM2610 sample blobs
- 0x4000-byte sprite RAM window with S.C.I.'s page selector
- S.C.I. 16x8 sprite chunk renderer using the parent spritemap

MAME is treated as the hardware map and ROM-load reference, not as the final
behavior target. MAME currently flags S.C.I. with imperfect graphics, so the
native renderer should be measured against real game behavior and improved where
the existing emulation path is weak.

Build sequence:

```sh
./sci_extract_roms.py
bash build_host.sh
/tmp/sci_hosttest 60
bash build_rtg.sh
bash package_sci_folder.sh
```

The Amiga RTG binary is written to `../dist/S.C.I. v2`. The Amiberry boot
volume is installed as the host folder
`/home/jon/Amiberry/HardDrives/SCI_RTG_HD` and config
`/home/jon/Amiberry/Configurations/SCI-RTG.uae`.
