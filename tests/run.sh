#!/bin/sh
# Host tests of the pure logic in NativeMsdc.h.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined "$here/VerifiedPolicyTests.cpp" -o "$out/verified-policy"
"$out/verified-policy"
