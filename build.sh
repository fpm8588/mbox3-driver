#!/usr/bin/env bash
# build.sh — builds everything with the free Xcode Command Line Tools (xcode-select --install).
# No Xcode project, no paid Apple Developer account, no notarization. Ad-hoc signing only.
#
#   ./build.sh test     hardware-free unit tests (also runs on Linux)
#   ./build.sh cli      build/mbox3ctl (arm64, macOS 13+)
#   ./build.sh all      test + cli (+ driver once Phase 2 lands)
#   ./build.sh clean
set -euo pipefail
cd "$(dirname "$0")"

B=build
MINOS=13.0
ARCH=${ARCH:-arm64}          # Apple Silicon only; set ARCH="arm64 x86_64" for a universal build
CFLAGS=(-std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -I Sources/Shared)

mkdir -p "$B"

run_tests() {
    local cc=${CC:-cc}
    echo "== unit tests ($cc)"
    $cc "${CFLAGS[@]}" Tests/test_shared.c -lm -o "$B/test_shared"
    "$B/test_shared"
    $cc "${CFLAGS[@]}" Tests/test_descdump.c Sources/mbox3ctl/descdump.c -o "$B/test_descdump"
    "$B/test_descdump" > /dev/null && echo "descdump: OK"
}

build_cli() {
    [[ "$(uname)" == "Darwin" ]] || { echo "mbox3ctl needs macOS (IOKit)"; exit 1; }
    echo "== mbox3ctl ($ARCH)"
    local archflags=()
    for a in $ARCH; do archflags+=(-arch "$a"); done
    clang "${CFLAGS[@]}" "${archflags[@]}" -mmacosx-version-min=$MINOS \
        -Wno-deprecated-declarations \
        Sources/mbox3ctl/main.c Sources/mbox3ctl/descdump.c \
        -framework IOKit -framework CoreFoundation \
        -o "$B/mbox3ctl"
    codesign --force --sign - "$B/mbox3ctl"
    file "$B/mbox3ctl"
}

case "${1:-all}" in
    test)  run_tests ;;
    cli)   build_cli ;;
    all)   run_tests; [[ "$(uname)" == "Darwin" ]] && build_cli || true ;;
    clean) rm -rf "$B" ;;
    *)     echo "usage: $0 test|cli|all|clean"; exit 2 ;;
esac
