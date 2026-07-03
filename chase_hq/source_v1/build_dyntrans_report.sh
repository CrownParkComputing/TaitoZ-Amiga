#!/bin/bash
# Build the host-only Chase HQ 68000 dyntrans coverage report.
set -e
cd "$(dirname "$0")"
gcc -O2 \
    -I. \
    -Icores \
    -I../../../Sega_System16/Shinobi/source/tools \
    tools/cc_dyntrans_report.c \
    tools/cc_xlate_plan.c \
    ../../../Sega_System16/Shinobi/source/tools/shinobi_xlate.c \
    cores/m68kdasm.c \
    -o /tmp/cc_dyntrans_report
echo "built /tmp/cc_dyntrans_report"
