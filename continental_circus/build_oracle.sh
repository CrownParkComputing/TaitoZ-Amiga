#!/bin/bash
set -e
cd "$(dirname "$0")"
gcc -O2 -I. -o /tmp/claude-1000/-home-jon-AmigaArcadePorts/6207b065-fbcb-4925-b26c-a38a6c4c8ce7/scratchpad/oracle/cc_oracle_render cc_render.c cc_oracle_render.c
echo "built oracle renderer"
