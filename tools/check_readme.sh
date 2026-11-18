#!/usr/bin/env bash
# Verifies that every ```cpp block in README.md still compiles against mapdb.hpp.
#
# The quick-start blocks build on each other (they declare structs, then tables,
# then queries), so they are concatenated in document order into one program.
# Code fences under "Set Traversal" are a full program and are skipped: that
# example lives in examples/advanced_demo.cpp and is built by `make demo`.
set -euo pipefail

cd "$(dirname "$0")/.."

OUT=$(mktemp /tmp/mapdb_readme_XXXXXX.cpp)
RUNDIR=$(mktemp -d /tmp/mapdb_readme_run_XXXXXX)
trap 'rm -rf "$OUT" "$OUT.o" "$RUNDIR"' EXIT

python3 - "$OUT" <<'PY'
import re, sys

readme = open('README.md', encoding='utf-8').read()
start = readme.index('## Include MapDB')
end = readme.index('## Complete Program')
blocks = re.findall(r'```cpp\n(.*?)```', readme[start:end], re.S)

includes, structs, body = [], [], []
for block in blocks:
    if '#include' in block and 'struct' not in block:
        includes.append(block)
    elif re.match(r'\s*struct ', block):
        structs.append(block)
    else:
        body.append(block)

source = "\n".join(includes) + "\n" + "\n".join(structs) + "\nint main() {\n" + "\n".join(body) + "\nreturn 0;\n}\n"
open(sys.argv[1], 'w', encoding='utf-8').write(source)
print(f"readme blocks: {len(blocks)} ({len(includes)} includes, {len(structs)} structs, {len(body)} statements)")
PY

g++ -std=c++17 -Wall -Wextra -Werror -I. -c "$OUT" -o "$OUT.o"
echo "README code blocks compile cleanly"

# Link and run it too, so the samples are proven to execute, not just parse.
g++ -std=c++17 -I. "$OUT" -o "$RUNDIR/readme_example" -pthread
(cd "$RUNDIR" && ./readme_example > /dev/null)
echo "README example program runs cleanly"
