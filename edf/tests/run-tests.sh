#!/bin/sh
# Builds and runs the offline test suites of the edf plugin, one at a time (make -j2).
# No meter, no serial hardware, no network.
#   edf/tests/run-tests.sh [build directory, default /tmp/edf-tests]
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
BUILD=${1:-/tmp/edf-tests}
QMAKE=${QMAKE:-qmake6}
status=0
for suite in tic contract port; do
    mkdir -p "$BUILD/$suite"
    (cd "$BUILD/$suite" && $QMAKE "$SRC/$suite/$suite.pro" >/dev/null && make -j2 >/dev/null)
    binary=$(ls "$BUILD/$suite"/tst_* | head -1)
    echo "== $suite"
    if ! (cd "$BUILD/$suite" && timeout -s KILL 300 "$binary" 2>&1 | grep -E "^(FAIL|Totals)|Loc:"); then :; fi
    (cd "$BUILD/$suite" && timeout -s KILL 300 "$binary" -silent >/dev/null 2>&1) || status=1
done
exit $status
