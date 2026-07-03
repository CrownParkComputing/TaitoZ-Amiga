#!/bin/bash
# Regenerate Chase HQ dyntrans coverage and report from the host reference path.
set -e
cd "$(dirname "$0")/.."
frames="${1:-6000}"
cov="${2:-/tmp/chq_pc_coverage.txt}"

bash build_host_coverage.sh
CHQ_COVERAGE="$cov" \
CC_COIN=1 \
CHQ_COIN_FRAME=60 \
CHQ_START_FRAME=120 \
CHQ_GAS_FRAME=260 \
/tmp/cc_hostcoverage "$frames"

bash build_dyntrans_report.sh
/tmp/cc_dyntrans_report "$cov"
