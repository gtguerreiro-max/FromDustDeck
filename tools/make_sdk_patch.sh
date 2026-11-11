#!/usr/bin/env bash
# Regenerates patches/fromdust/0001-fromdust-sdk-changes.patch: the difference between
# build/rexglue-sdk and upstream ReXGlue (patches/rexglue.lock) with patches/deck applied, made
# in a temporary git worktree. Then checks that upstream + deck + fromdust reproduces the tree.
#   tools/make_sdk_patch.sh        (Git Bash on Windows or Linux)
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk=$repo/build/rexglue-sdk
tmp=$repo/build/base-tmp
out=$repo/patches/fromdust/0001-fromdust-sdk-changes.patch
source <(tr -d '\r' < "$repo/patches/rexglue.lock")
git -C "$sdk" worktree remove --force "$tmp" 2>/dev/null || rm -rf "$tmp"
git -C "$sdk" worktree add -q --detach "$tmp" "$REXGLUE_COMMIT"
for p in "$repo"/patches/deck/*.patch; do git -C "$tmp" apply --whitespace=nowarn "$p"; done
(cd "$repo/build" && diff -ruN -x out -x .git -x thirdparty -x bin -x '*.orig' -x '*.rej' \
  base-tmp rexglue-sdk > own.diff) || true
py=python3; python3 -c "" 2>/dev/null || py=python
"$py" - "$repo/build/own.diff" "$out" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="utf-8", errors="surrogateescape", newline="").read()
out = []
for p in re.split(r"(?m)^(?=diff -ruN )", s):
    if not p.startswith("diff -ruN"):
        continue
    path = re.search(r"rexglue-sdk/(\S+)\s*$", p.split("\n", 1)[0]).group(1)
    lines = p.split("\n")
    new = "1970-01-01" in lines[1]
    hdr = "diff --git a/%s b/%s\n" % (path, path)
    hdr += "new file mode 100644\n--- /dev/null\n" if new else "--- a/%s\n" % path
    out.append(hdr + "+++ b/%s\n" % path + "\n".join(lines[3:]))
open(sys.argv[2], "w", encoding="utf-8", errors="surrogateescape", newline="").write("".join(out))
print(len(out), "files")
EOF
git -C "$tmp" apply --whitespace=nowarn "$out"
if diff -rq -x out -x .git -x thirdparty -x bin "$tmp" "$sdk"; then echo "round trip OK"; fi
git -C "$sdk" worktree remove --force "$tmp"
rm -f "$repo/build/own.diff"
