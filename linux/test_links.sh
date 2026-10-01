#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# The two symlink creations in nvmet_setup.sh, against a fake configfs whose `ln`
# behaves like the real one.
#
# This exists because the first version of these guards was validated with a fake tree
# on which `ln -sfn` successfully REPLACES an existing link.  configfs does not
# support unlink, so `-f` cannot remove the link it is replacing and the call fails
# with EEXIST - which under `set -e` ends the run.  The re-run therefore died on the
# peer's real target while every local test was green.  Overriding `ln` to refuse an
# existing destination is what makes the local test tell the truth.
set -e
SRC=/tmp/nvmet_setup.sh

start=$(grep -n '^set_subsys_allow_any_host() {' "$SRC" | head -1 | cut -d: -f1)
end=$(grep -n '^mkdir -p "\$CONF/subsystems/\$SUBNQN/namespaces/1"' "$SRC" | head -1 | cut -d: -f1)
end=$((end - 1))
echo "block: lines $start..$end"
[ -n "$start" ] && [ -n "$end" ] && [ "$end" -gt "$start" ] || { echo "FAIL: block not found"; exit 1; }

{
    cat <<'X'
CONF="$1"; SUBNQN="$2"; AUTH_KEY="$3"; AUTH_HOST_NQN="$4"
AUTH_KEY_FILE=""; AUTH_CTRL_KEY=""; AUTH_CTRL_KEY_FILE=""
AUTH_HASH=sha256; AUTH_DHGROUP=ffdhe2048
# configfs semantics: an existing symbolic link cannot be replaced.
ln() {
    eval "last=\${$#}"
    if [ -e "$last" ] || [ -L "$last" ]; then
        echo "ln: failed to create symbolic link '$last': File exists" >&2
        return 1
    fi
    command ln "$@"
}
X
    head -"$end" "$SRC" | tail -n +"$start"
} > /tmp/linkblock.sh

mkfake() {
    rm -rf "$1"
    mkdir -p "$1/hosts" "$1/subsystems/$2/allowed_hosts"
    printf '0\n' > "$1/subsystems/$2/attr_allow_any_host"
}
NQN=nqn.2024-01.local.rdma:linux-nvmet
HOST=nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001
KEY='DHHC-1:01:AAAA:'

pass=0; fail=0
check() { case "$3" in *"$2"*) echo "  [ok] $1"; pass=$((pass+1)) ;;
                       *) echo "  [FAIL] $1 - got: $3"; fail=$((fail+1)) ;; esac; }

mkfake /tmp/lf1 "$NQN"
out=$(sh /tmp/linkblock.sh /tmp/lf1 "$NQN" "$KEY" "$HOST" 2>&1); rc=$?
check "first run: success" "host linked into allowed_hosts" "$out"
[ "$rc" -eq 0 ] && { echo "  [ok] first run: exit 0"; pass=$((pass+1)); } \
                || { echo "  [FAIL] first run: exit $rc"; fail=$((fail+1)); }
[ -L "/tmp/lf1/subsystems/$NQN/allowed_hosts/$HOST" ] \
    && { echo "  [ok] first run: the link exists"; pass=$((pass+1)); } \
    || { echo "  [FAIL] first run: no link"; fail=$((fail+1)); }

# THE CASE THAT MATTERS: the same run again, over the link it created.  Without the
# guard this is "ln: ... File exists" and a non-zero exit.
out=$(sh /tmp/linkblock.sh /tmp/lf1 "$NQN" "$KEY" "$HOST" 2>&1); rc=$?
check "second run: the existing link is left alone" "already linked into allowed_hosts" "$out"
[ "$rc" -eq 0 ] && { echo "  [ok] second run: exit 0"; pass=$((pass+1)); } \
                || { echo "  [FAIL] second run: exit $rc (configfs EEXIST not handled)"; fail=$((fail+1)); }
case "$out" in *"File exists"*) echo "  [FAIL] second run: tried to replace the link"; fail=$((fail+1)) ;; \
                *) echo "  [ok] second run: never attempted a replace"; pass=$((pass+1)) ;; esac

# And a third, to be sure it is idempotent rather than merely twice-tolerant.
out=$(sh /tmp/linkblock.sh /tmp/lf1 "$NQN" "$KEY" "$HOST" 2>&1); rc=$?
[ "$rc" -eq 0 ] && { echo "  [ok] third run: exit 0"; pass=$((pass+1)); } \
                || { echo "  [FAIL] third run: exit $rc"; fail=$((fail+1)); }

echo "link guards: $pass ok, $fail failed"
[ "$fail" -eq 0 ]
