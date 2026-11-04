#!/bin/bash
# Builds and runs the DuckSleep host tests with AddressSanitizer and UBSan.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${TMPDIR:-/tmp}/ducksleep-host"
mkdir -p "$out"

cxx="${CXX:-clang++}"
flags=(-std=c++17 -g -O1 -Wall -Wextra -Wshadow -Werror
       -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
       -I "$root/src" -I "$here")

echo "== SleepPolicy"
"$cxx" "${flags[@]}" "$here/policy_test.cpp" -o "$out/policy_test"
"$out/policy_test"
