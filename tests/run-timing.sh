#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-pie -no-pie "$here/VerifiedTimingTests.cpp" -o "$out/timing"
"$out/timing"
