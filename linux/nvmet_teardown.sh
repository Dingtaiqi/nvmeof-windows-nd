#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  nvmet_teardown.sh - undo nvmet_setup.sh, in the reverse order.
#
#  Order matters in configfs: the port's symlink to the subsystem has to go
#  before the subsystem, and a namespace has to be disabled before it is
#  removed, or the rmdir fails with EBUSY and leaves a half-configured target
#  behind for the next run.
#
#  Two things this script learned the hard way (both found while testing
#  DH-HMAC-CHAP on a peer):
#
#   * allowed_hosts/ entries are part of the subsystem's directory, so a subsystem
#     that has one CANNOT be removed - rmdir answers ENOTEMPTY, and this script used
#     to swallow that with `|| true`.  The old subsystem then survived with its host
#     list intact, and the next setup run died on EINVAL ("Can't set allow_any_host
#     when explicit hosts are set!") before it configured anything.  A teardown that
#     reports success while leaving the subsystem behind is worse than one that
#     fails, so both are fixed below: the entries are removed, and the end state is
#     verified instead of assumed.
#
#   * $SUBNQN is pasted into every path, and this script is one of the commands a
#     NOPASSWD sudoers rule allows.  `nvmet_teardown.sh ../../../../etc/passwd`
#     therefore reaches `rm -f` outside configfs: an arbitrary-root-file-deletion
#     primitive.  The rule is exact-path by design and should not be widened, so the
#     validation lives here.
# ===========================================================================
set -e

SUBNQN="${1:-nqn.2024-01.local.rdma:linux-nvmet}"
CONF=/sys/kernel/config/nvmet

# Input validation comes FIRST, before the root check, for two reasons: a bad
# argument should be rejected whether or not the caller is root, and this is the
# only part of the script that can be tested without a password.
case "$SUBNQN" in
    nqn.*) ;;
    *) echo "refusing: '$SUBNQN' is not an NQN (it must start with 'nqn.')" >&2; exit 1 ;;
esac
case "$SUBNQN" in
    */*|*..*)
        echo "refusing: '$SUBNQN' contains '/' or '..' - it is used to build paths" >&2
        exit 1 ;;
esac

if [ "$(id -u)" != "0" ]; then
    echo "must run as root" >&2
    exit 1
fi

echo "== disconnecting any host still attached"
if command -v nvme >/dev/null 2>&1; then
    nvme disconnect -n "$SUBNQN" 2>/dev/null || true
fi

echo "== port"
rm -f "$CONF/ports/1/subsystems/$SUBNQN" 2>/dev/null || true
rmdir "$CONF/ports/1" 2>/dev/null || true

echo "== allowed hosts (they live inside the subsystem and block its removal)"
if [ -d "$CONF/subsystems/$SUBNQN/allowed_hosts" ]; then
    for h in "$CONF/subsystems/$SUBNQN/allowed_hosts"/*; do
        [ -e "$h" ] || continue
        echo "   removing $(basename "$h")"
        rm -f "$h"
    done
fi

echo "== namespace and subsystem"
if [ -d "$CONF/subsystems/$SUBNQN/namespaces/1" ]; then
    echo 0 > "$CONF/subsystems/$SUBNQN/namespaces/1/enable" 2>/dev/null || true
    rmdir "$CONF/subsystems/$SUBNQN/namespaces/1" 2>/dev/null || true
fi
rmdir "$CONF/subsystems/$SUBNQN" 2>/dev/null || true

# Verify instead of assuming: every step above is `|| true` (configfs returns
# different errors depending on what is left over), so the only honest statement
# about the outcome is one made after looking.
rc=0
if [ -d "$CONF/subsystems/$SUBNQN" ]; then
    echo "FAIL: the subsystem is still there - it was not torn down" >&2
    echo "      contents left behind:" >&2
    ls -la "$CONF/subsystems/$SUBNQN" 2>&1 | sed 's/^/        /' >&2
    echo "      (a controller may still be attached: nvme list | grep -i linux)" >&2
    rc=1
fi
if [ -d "$CONF/ports/1" ]; then
    echo "FAIL: port 1 is still there" >&2
    ls -la "$CONF/ports/1" 2>&1 | sed 's/^/        /' >&2
    rc=1
fi
[ "$rc" -eq 0 ] && echo "teardown verified: subsystem and port are gone"

echo "== modules left loaded on purpose (nvmet, nvmet-rdma, brd)"
if [ -n "${RXE_NETDEV:-}" ] || command -v rdma >/dev/null 2>&1; then
    # Drop the software RoCE device if this run created one.  Harmless if absent.
    rdma link del rxe0 2>/dev/null && echo "   removed rxe0" || true
fi
echo "done (rc=$rc)"
exit "$rc"
