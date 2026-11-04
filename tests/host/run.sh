#!/bin/bash
# Builds and runs the DuckSleep host tests with AddressSanitizer and UBSan:
#  - SleepPolicy (pure decisions) and SleepStatusMessage (payload builder)
#  - the public header, compiled with the defaults (inline no-ops) and with
#    sleep enabled (declarations only)
#  - every out-of-range define must fail to compile
#
# Needs ArduinoJson 7 sources for the payload test. Set ARDUINOJSON_SRC to its
# src/ folder, or build the example once ('pio run') so .pio/libdeps has it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/ducksleep-host"
mkdir -p "$out"

json="${ARDUINOJSON_SRC:-}"
if [[ -z "$json" ]]; then
    json="$(ls -d "$root"/.pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -1 || true)"
fi
if [[ -z "$json" || ! -f "$json/ArduinoJson.h" ]]; then
    echo "ArduinoJson 7 sources not found. Set ARDUINOJSON_SRC to ArduinoJson/src," >&2
    echo "or run 'pio run' in $root once so .pio/libdeps has them." >&2
    exit 1
fi

cxx="${CXX:-clang++}"
flags=(-std=c++17 -g -O1 -Wall -Wextra -Wshadow -Werror
       -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
       -I "$root/src" -I "$here" -isystem "$json")

echo "== SleepPolicy"
"$cxx" "${flags[@]}" "$here/policy_test.cpp" -o "$out/policy_test"
"$out/policy_test"

echo "== SleepStatusMessage"
"$cxx" "${flags[@]}" "$here/status_test.cpp" -o "$out/status_test"
"$out/status_test"

echo "== DuckSleep.h with the defaults"
"$cxx" "${flags[@]}" "$here/api_test.cpp" -o "$out/api_test"
"$out/api_test"

echo "== DuckSleep.h with sleep and survival enabled (compile only)"
"$cxx" "${flags[@]}" -fsyntax-only -DDUCK_SLEEP_ENABLED=1 -DDUCK_SLEEP_START_MIN_UTC=1320 \
    -DDUCK_SLEEP_WAKE_MIN_UTC=360 -DDUCK_SLEEP_ROLE=0 -DDUCK_SURVIVAL_ENABLED=1 \
    -DDUCK_BUILD_EPOCH=1793750400 "$here/api_test.cpp"
echo "PASS DuckSleep.h (enabled) compiles"

echo "== Out-of-range defines must not compile"
bad=(
    "-DDUCK_SLEEP_ENABLED=2"
    "-DDUCK_SLEEP_START_MIN_UTC=1440"
    "-DDUCK_SLEEP_WAKE_MIN_UTC=-1"
    "-DDUCK_SLEEP_ROLE=2"
    "-DDUCK_SLEEP_WAKE_JITTER_S=601"
    "-DDUCK_SLEEP_MAX_CHUNK_S=59"
    "-DDUCK_SLEEP_MAX_CHUNK_S=86401"
    "-DDUCK_SURVIVAL_ENABLED=3"
    "-DDUCK_SURVIVAL_SLEEP_MV=2999"
    "-DDUCK_SURVIVAL_WAKE_MV=4201"
    "-DDUCK_SURVIVAL_WAKE_MV=3400"
    "-DDUCK_SURVIVAL_RECHECK_MIN=4"
    "-DDUCK_SLEEP_STATUS_TOPIC=0x0F"
    "-DDUCK_BUILD_EPOCH=12345"
)
# Control: the same command with valid values compiles, so a rejection below
# comes from the range checks and not from the command itself.
"$cxx" -std=c++17 -fsyntax-only -I "$root/src" -DDUCK_SURVIVAL_WAKE_MV=3401 -DDUCK_BUILD_EPOCH=1793750400 \
    -x c++ - <<<'#include "DuckSleepConfig.h"'
rejected=0
for define in "${bad[@]}"; do
    if "$cxx" -std=c++17 -fsyntax-only -I "$root/src" "$define" -x c++ - <<<'#include "DuckSleepConfig.h"' 2>/dev/null; then
        echo "FAIL accepted: $define"
        exit 1
    fi
    rejected=$((rejected + 1))
done
echo "PASS ${rejected} out-of-range defines rejected"
