#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-pie -no-pie "$here/VerifiedCacheTests.cpp" -o "$out/cache"
"$out/cache"
${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-pie -no-pie "$here/VerifiedCacheConcurrencyTests.cpp" -o "$out/concurrency"
"$out/concurrency"
python3 "$here/check-cache-integration.py"
