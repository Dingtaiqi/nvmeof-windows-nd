#!/bin/sh
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  nvmet_setup.sh - turn a Linux box into an NVMe-oF/RDMA target for F5.
#
#  This is the independent peer the whole project has been missing: Linux's own
#  nvmet, driven by configfs, with no code of ours on that side.  Once this is
#  running, our initiator is talking to somebody else's implementation - which
#  is the only test that can find a mistake both of our own endpoints agree on.
#
#  Usage (as root):
#      ./nvmet_setup.sh <local-ip> [subnqn] [port] [size-mb] [auth-key-file]
#  Defaults:
#      subnqn  nqn.2024-01.local.rdma:linux-nvmet
#      port    4420        (NVME_RDMA_IP_PORT)
#      size    64 MiB      (a ramdisk; no filesystem involved)
#      auth    none        (with a key file: DH-HMAC-CHAP is REQUIRED - see below)
#
#  No RDMA-capable NIC?  Use software RoCE on any ordinary interface:
#      RXE_NETDEV=ens18 ./nvmet_setup.sh 192.168.100.2
#  (See the notes at the rxe block below for the one hard requirement.)
#
#  Packages per distribution (only what this script calls):
#      Arch      pacman -S --needed iproute2 rdma-core nvme-cli
#      Debian    apt install iproute2 rdma-core nvme-cli
#      Fedora    dnf install iproute rdma-core nvme-cli
#  'rdma' comes from iproute2; the kernel side is nvmet, nvmet-rdma, rdma_rxe, brd.
#  Arch ships no firewall by default; on other systems the script opens UDP 4791.
#
#  Then, on Windows:
#      run_f5.ps1 -initiatorOnly -subnqn <subnqn> -serverIp <local-ip> -port <port>
#
#  Everything this script configures is in configfs and disappears on teardown
#  (see nvmet_teardown.sh).  Nothing is installed and nothing is formatted.
# ===========================================================================
set -e

IP="${1:?usage: nvmet_setup.sh <local-ip> [subnqn] [port] [size-mb] [auth-key-file] [block-device]}"
SUBNQN="${2:-nqn.2024-01.local.rdma:linux-nvmet}"
PORT="${3:-4420}"
SIZE_MB="${4:-64}"
# The 6th argument exports an EXISTING BLOCK DEVICE instead of a ramdisk.  It is a
# positional argument and not an environment variable on purpose: the sudoers entry
# names this script by exact path, sudo resets the environment, and a block device
# name that silently falls back to "the ramdisk" is exactly the kind of default that
# ends with the wrong disk exported.  See the rails below.
NSDEV="${6:-}"
# A 6th argument can be skipped by passing "-" for the key file, so both of these
# mean "no device".
[ "$NSDEV" = "-" ] && NSDEV=""

CONF=/sys/kernel/config/nvmet
RAMSIZE_KB=$((SIZE_MB * 1024))

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (configfs writes)" >&2
    exit 1
fi
if [ ! -d /sys/kernel/config ]; then
    mount -t configfs none /sys/kernel/config
fi

echo "== modules"
modprobe nvmet
modprobe nvmet-rdma
# The back end: a ramdisk, so the namespace has no filesystem and no block size
# of its own to disagree about.  brd gives /dev/ram0 of the requested size.
# (Skipped when a real device is being exported - the ramdisk would be pointless.)
[ -n "$NSDEV" ] || modprobe brd rd_nr=1 rd_size="$RAMSIZE_KB" || true
lsmod | grep -E '^nvmet' || { echo "nvmet did not load" >&2; exit 1; }

# ---------------------------------------------------------------------------
#  Exporting a REAL DISK: the rails come first, before anything is created.
#
#  Every rail here exists because the failure it prevents is unrecoverable:
#  exporting a device that something is using gives two writers to one filesystem,
#  and exporting the boot device of the machine doing the exporting is worse.  A
#  namespace backed by a real block device is not a scratch namespace, and this
#  script must not be one careless argument away from destroying a disk.
# ---------------------------------------------------------------------------
if [ -n "$NSDEV" ]; then
    case "$NSDEV" in
        /dev/nvme[0-9]*n[0-9]*|/dev/nvme[0-9]*n[0-9]*p[0-9]*|/dev/sd[a-z]*[0-9]*|/dev/vd[a-z]*[0-9]*) ;;
        *) echo "refusing '$NSDEV': not a whole-disk or partition device path" >&2; exit 1 ;;
    esac
    [ -b "$NSDEV" ] || { echo "refusing '$NSDEV': not a block device" >&2; exit 1; }

    # 1. Nothing may have it open, and nothing may have any of its partitions.
    if [ -n "$(ls /sys/block/$(basename "$NSDEV")/holders 2>/dev/null)" ]; then
        echo "refusing '$NSDEV': it has holders ($(ls /sys/block/$(basename "$NSDEV")/holders | tr '\n' ' '))" >&2
        exit 1
    fi
    if findmnt -n -S "$NSDEV" >/dev/null 2>&1; then
        echo "refusing '$NSDEV': it is mounted right now" >&2
        exit 1
    fi
    for p in "$NSDEV"p*; do
        [ -b "$p" ] || continue
        if findmnt -n -S "$p" >/dev/null 2>&1; then
            echo "refusing '$NSDEV': partition $p is mounted right now" >&2
            exit 1
        fi
        if [ -n "$(ls /sys/block/$(basename "$p")/holders 2>/dev/null)" ]; then
            echo "refusing '$NSDEV': partition $p has holders" >&2
            exit 1
        fi
    done

    # 2. It must not be the device the running system lives on.
    rootdev=$(findmnt -n -o SOURCE / | sed 's/\[.*//')
    rootparent=$(lsblk -n -o PKNAME "$rootdev" 2>/dev/null | head -1)
    cur=$(basename "$NSDEV")
    if [ "$cur" = "$(basename "$rootdev")" ] || [ "$cur" = "$rootparent" ]; then
        echo "refusing '$NSDEV': this is the device the running system boots from" >&2
        exit 1
    fi

    echo "== EXPORTING A REAL DISK"
    echo "   device : $NSDEV"
    echo "   size   : $(lsblk -n -d -o SIZE "$NSDEV" 2>/dev/null)"
    echo "   model  : $(lsblk -n -d -o MODEL "$NSDEV" 2>/dev/null)"
    echo "   serial : $(lsblk -n -d -o SERIAL "$NSDEV" 2>/dev/null)"
    echo "   layout :"
    lsblk -n -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT "$NSDEV" 2>/dev/null | sed 's/^/     /'
    echo "   NOTE: nvmet has no read-only namespace attribute.  Whoever connects can"
    echo "         write to this disk; the host side is responsible for not doing it."
fi


# ---- software RoCE (rxe) when there is no RDMA-capable NIC ----
#
# RXE_NETDEV=<iface> turns the ordinary NIC named by <iface> into a software RoCE
# (RoCEv2) device.  That is enough to be an independent peer: rxe is a complete,
# separate implementation of the same protocol, living in the Linux kernel, and it
# interoperates with hardware RoCE - which is what the Windows side is.  What it
# does NOT reproduce is hardware timing and throughput, so treat F5's numbers from
# this path as correctness evidence, not as performance evidence.
#
#     RXE_NETDEV=ens18 ./nvmet_setup.sh 192.168.100.2
#
# The one hard requirement either way: the interface must be in the SAME SUBNET as
# the Windows RoCE port, so that ARP resolves the peer directly.  RoCE is not
# routable here - a gateway between the two will not work.
if [ -n "${RXE_NETDEV:-}" ]; then
    echo "== software RoCE on $RXE_NETDEV"
    modprobe rdma_rxe || { echo "rdma_rxe did not load (kernel module missing?)" >&2; exit 1; }
    ip link show "$RXE_NETDEV" >/dev/null 2>&1 || {
        echo "no such interface: $RXE_NETDEV" >&2; ip -brief link show >&2; exit 1; }
    # Adding an rxe link to the same netdev twice fails; that is fine and not an error.
    rdma link add rxe0 type rxe netdev "$RXE_NETDEV" 2>/dev/null || true
    rdma link show | sed 's/^/   /'
    if ! rdma link show 2>/dev/null | grep -q rxe; then
        echo "rxe is not up.  'rdma' from iproute2 is required (iproute2 package)." >&2
        exit 1
    fi
    # RoCEv2 is UDP 4791.  A default-deny firewall silently drops it and the symptom
    # is a connect that times out with nothing in any log.
    if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
        firewall-cmd --add-port=4791/udp >/dev/null 2>&1 && echo "   firewalld: opened 4791/udp" || true
    elif command -v ufw >/dev/null 2>&1; then
        ufw allow 4791/udp >/dev/null 2>&1 && echo "   ufw: allowed 4791/udp" || true
    fi
fi

# The RDMA device must be up and have an IP: nvmet-rdma binds an RDMA-CM id to
# the address below, and RoCE needs the netdev in the same broadcast domain as
# the Windows box.
echo "== rdma devices"
if command -v ibv_devices >/dev/null 2>&1; then ibv_devices; fi
ip -brief addr show | sed 's/^/   /'

echo "== subsystem $SUBNQN"
mkdir -p "$CONF/subsystems/$SUBNQN"
# allow_any_host is NOT set here.  It is not a free switch in either direction:
#
#   * setting it to 1 while allowed_hosts/ has entries is refused outright -
#     "Can't set allow_any_host when explicit hosts are set!" (EINVAL);
#   * adding a host while it is 1 is refused the other way round -
#     "can't add hosts when allow_any_host is set!" (EINVAL).
#
# So the two have to move in a fixed ORDER, and the order depends on which state we
# are heading for.  This script used to `echo 1 > attr_allow_any_host` right here,
# before it knew whether a key had been configured: the first run worked (the list
# was empty), and every LATER run died on EINVAL before it ever reached the auth
# block - so a target that was already authenticating could not be reconfigured.
# Both transitions now live in the auth block below, with the list and the flag
# always changed in the only order the kernel accepts.
#
# allow_any_host: our host NQN is nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001.

set_subsys_allow_any_host() { # <0|1>; writes only when the value differs
    cur=$(cat "$CONF/subsystems/$SUBNQN/attr_allow_any_host" 2>/dev/null || true)
    [ "$cur" = "$1" ] && return 0
    printf '%s' "$1" > "$CONF/subsystems/$SUBNQN/attr_allow_any_host"
}

# ---- DH-HMAC-CHAP in-band authentication (optional) ----
#
#     AUTH_KEY='DHHC-1:01:<base64>:' ./nvmet_setup.sh 192.168.100.5
#
# Setting AUTH_KEY does three things that are easy to get half-right, so all three
# are done here together:
#
#   1. create /sys/kernel/config/nvmet/hosts/<AUTH_HOST_NQN> and put the key on it.
#      The attribute wants the WHOLE "DHHC-1:<hh>:<base64>" string - nvmet_auth_set_key()
#      parses the prefix itself and stores from `secret + 10`.
#   2. turn attr_allow_any_host OFF.  This is not a security nicety, it is the
#      switch that enables authentication at all: nvmet_setup_auth() returns before
#      it looks at any key when allow_any_host is set, so a target with a perfectly
#      good host key above and allow_any_host=1 never sets ATR and never asks.
#   3. link the host into the subsystem's allowed_hosts/, which is how nvmet finds
#      it: it walks <subsys>/hosts looking for an NQN equal to the one in the
#      Connect data.
#
# Without AUTH_KEY the script undoes all three, so the same command brings the peer
# back to the no-authentication configuration the other interop runs expect.
#
# The key is read from a FILE, and the file is named either by the fifth positional
# argument or by AUTH_KEY_FILE.  It is never read from the command line and never
# from a command-line environment assignment:
#
#   * a key on the command line lands in `ps` output and in shell history, and a
#     DH-HMAC-CHAP secret is a long-lived credential;
#   * `sudo -n VAR=x cmd` does not work at all here anyway.  The sudoers rule that
#     makes this script runnable is a bare path with no SETENV, so the command sudo
#     matches is `env`, which is not in the rule - the point is moot in theory and a
#     hard failure in practice.
#
# The fifth argument is a PATH, not a secret, so it is safe on a command line:
#
#     sudo -n <this script> <ip> <subnqn> <port> <size> ~/.f5_nvmet_auth.key
#
# First line of the file = host key, second line = controller key (optional).
AUTH_KEY="${AUTH_KEY:-}"
AUTH_KEY_FILE="${5:-${AUTH_KEY_FILE:-}}"
AUTH_CTRL_KEY="${AUTH_CTRL_KEY:-}"
AUTH_CTRL_KEY_FILE="${AUTH_CTRL_KEY_FILE:-}"
AUTH_HOST_NQN="${AUTH_HOST_NQN:-nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001}"
AUTH_HASH="${AUTH_HASH:-sha256}"
AUTH_DHGROUP="${AUTH_DHGROUP:-ffdhe2048}"

# "-" or "none" as the fifth argument means "explicitly no authentication", which is
# how a caller says "I know this target has a key file, turn it off anyway".
case "$AUTH_KEY_FILE" in
    -|none) AUTH_KEY_FILE=""; AUTH_KEY="" ;;
esac

if [ -z "$AUTH_KEY" ] && [ -n "$AUTH_KEY_FILE" ]; then
    [ -r "$AUTH_KEY_FILE" ] || { echo "cannot read the key file $AUTH_KEY_FILE" >&2; exit 1; }
    # First non-empty line, CR stripped: the same file format the interop scripts use.
    AUTH_KEY=$(sed -n '1p' "$AUTH_KEY_FILE" | tr -d '\r\n')
    # ...and the second line is the controller key, so ONE path argument configures
    # both directions.  A file that has only one line means one-way authentication.
    if [ -z "$AUTH_CTRL_KEY" ] && [ -z "$AUTH_CTRL_KEY_FILE" ]; then
        AUTH_CTRL_KEY=$(sed -n '2p' "$AUTH_KEY_FILE" | tr -d '\r\n')
    fi
fi
if [ -z "$AUTH_CTRL_KEY" ] && [ -n "$AUTH_CTRL_KEY_FILE" ]; then
    [ -r "$AUTH_CTRL_KEY_FILE" ] || { echo "cannot read AUTH_CTRL_KEY_FILE=$AUTH_CTRL_KEY_FILE" >&2; exit 1; }
    AUTH_CTRL_KEY=$(sed -n '2p' "$AUTH_CTRL_KEY_FILE" | tr -d '\r\n')
fi
case "$AUTH_KEY" in
    "") ;;
    DHHC-1:*) ;;
    *) echo "AUTH_KEY does not start with DHHC-1:" >&2; exit 1 ;;
esac

if [ -n "$AUTH_KEY" ]; then
    # Never print the key itself, and never pass it to a command line: this only
    # ever reaches configfs through a shell redirect.
    echo "== DH-HMAC-CHAP: host key for $AUTH_HOST_NQN"
    mkdir -p "$CONF/hosts/$AUTH_HOST_NQN"
    # No trailing newline: nvmet strim()s it, but a key that round-trips byte for
    # byte is one less thing between here and a wrong HMAC.
    printf '%s' "$AUTH_KEY" > "$CONF/hosts/$AUTH_HOST_NQN/dhchap_key"
    if [ -n "$AUTH_CTRL_KEY" ]; then
        printf '%s' "$AUTH_CTRL_KEY" > "$CONF/hosts/$AUTH_HOST_NQN/dhchap_ctrl_key"
        echo "   controller key set (bidirectional authentication)"
    fi
    printf '%s' "$AUTH_HASH" > "$CONF/hosts/$AUTH_HOST_NQN/dhchap_hash" 2>/dev/null \
        && echo "   hash: $AUTH_HASH" || echo "   (this kernel has no dhchap_hash attribute)"
    # dhchap_dhgroup moved around between kernel versions and the ffdhe groups need
    # the dh kpp.  A failure here is not fatal: with no group configured nvmet does
    # NO Diffie-Hellman and just HMACs with the transformed key (the NULL group),
    # which is a legal exchange the host handles.  Say which one happened.
    if printf '%s' "$AUTH_DHGROUP" > "$CONF/hosts/$AUTH_HOST_NQN/dhchap_dhgroup" 2>/dev/null; then
        echo "   dhgroup: $AUTH_DHGROUP"
    else
        echo "   dhgroup: NOT settable here -> the NULL group (no DH, HMAC only)"
        AUTH_DHGROUP=""
    fi
    # ORDER: the flag goes to 0 BEFORE the host is linked.  Doing it the other way
    # round is refused by the kernel with "can't add hosts when allow_any_host is
    # set!" - and because the failure is a configfs write error under `set -e`, the
    # run ends there.
    if ! set_subsys_allow_any_host 0; then
        echo "   FAIL: cannot clear allow_any_host while hosts are already linked." >&2
        echo "         Remove them, then run this again:" >&2
        echo "           rm -f $CONF/subsystems/$SUBNQN/allowed_hosts/*" >&2
        exit 1
    fi
    mkdir -p "$CONF/subsystems/$SUBNQN/allowed_hosts"
    # `ln -sfn` is NOT idempotent here, and this is the third place in this file where
    # that had to be learned: configfs does not support unlink, so `-f` cannot remove
    # the link it is being asked to replace and the call fails with EEXIST - which,
    # under `set -e`, ends the run.  A plain filesystem hides this completely (`-f`
    # replaces the link and the fake-tree test passed), which is why it took a real
    # target with a real configfs to find it.
    auth_link="$CONF/subsystems/$SUBNQN/allowed_hosts/$AUTH_HOST_NQN"
    if [ -L "$auth_link" ]; then
        echo "   host already linked into allowed_hosts/"
    else
        ln -sfn "$CONF/hosts/$AUTH_HOST_NQN" "$auth_link"
        echo "   allow_any_host=0, host linked into allowed_hosts/"
    fi
    echo "   a host that connects without a key now fails with -ENOKEY;"
    echo "   one with the wrong key gets Failure1 and the controller is torn down"
else
    # Leave no half-configured auth behind: an allowed_hosts entry with a key on it
    # is enough to make a later non-auth run fail in a way that looks like a
    # transport bug.
    #
    # ORDER: the list is emptied BEFORE the flag goes back to 1.  The kernel refuses
    # allow_any_host=1 while explicit hosts exist ("Can't set allow_any_host when
    # explicit hosts are set!"), so the reverse order would leave the target in the
    # state this branch exists to prevent.
    if [ -d "$CONF/subsystems/$SUBNQN/allowed_hosts" ]; then
        for h in "$CONF/subsystems/$SUBNQN/allowed_hosts"/*; do
            [ -e "$h" ] || continue
            rm -f "$h"
        done
    fi
    set_subsys_allow_any_host 1 || {
        echo "   FAIL: could not set allow_any_host=1" >&2; exit 1; }
    echo "   auth: none (allow_any_host=1, allowed_hosts/ emptied)"
fi

mkdir -p "$CONF/subsystems/$SUBNQN/namespaces/1"
# device_path can be written only while the namespace is disabled: writing it again
# on an already-enabled namespace fails with EBUSY, and under `set -e` that aborts
# the rest of this script.  Re-running is normal (f5_linux_up.sh calls this every
# time), so only write it when it is not already what we want - and DISABLE first
# when it has to change, or switching this namespace from the ramdisk to a real disk
# aborts with a permissions error that says nothing about the real cause.
WANT_DEVPATH="${NSDEV:-/dev/ram0}"
HAVE_DEVPATH="$(cat "$CONF/subsystems/$SUBNQN/namespaces/1/device_path" 2>/dev/null)"
if [ "$HAVE_DEVPATH" != "$WANT_DEVPATH" ]; then
    if [ -n "$HAVE_DEVPATH" ]; then
        echo "   switching namespace 1 from '$HAVE_DEVPATH' to '$WANT_DEVPATH'"
        echo 0 > "$CONF/subsystems/$SUBNQN/namespaces/1/enable" 2>/dev/null || true
    fi
    echo -n "$WANT_DEVPATH" > "$CONF/subsystems/$SUBNQN/namespaces/1/device_path"
fi
echo 1 > "$CONF/subsystems/$SUBNQN/namespaces/1/enable"

echo "== port 1 on $IP:$PORT"
mkdir -p "$CONF/ports/1"

# Port attributes are WRITE-ONCE, not idempotent: the moment a subsystem is linked
# to the port, nvmet binds it (nvmet_enable_port) and every later write to
# addr_traddr / addr_trfams answers EACCES.  The unconditional `echo > addr_traddr`
# this script used to do therefore failed on every re-run, and under `set -e` it
# aborted the script right there - which is invisible in the common case, because
# the port already holds the right address and everything downstream is already
# configured.  It stops being invisible the moment somebody asks for a different
# address: the run "fails" with a permissions error that says nothing about why.
#
# So: compare first, write only when it differs, and when it differs AND the port is
# bound, say exactly what has to happen instead of dying with EACCES.
port_ok=1
set_port_attr() { # <attribute> <wanted>
    attr="$1"; want="$2"; path="$CONF/ports/1/$attr"
    cur=$(cat "$path" 2>/dev/null || true)
    [ "$cur" = "$want" ] && return 0
    if printf '%s' "$want" > "$path" 2>/dev/null; then
        echo "   $attr: '$cur' -> '$want'"
        return 0
    fi
    echo "   FAIL: $attr is '$cur' and cannot be changed to '$want' - the port is bound." >&2
    echo "         Unbind it first (this is the only way, and it drops the port):" >&2
    echo "           rm -f $CONF/ports/1/subsystems/*" >&2
    echo "           rmdir $CONF/ports/1" >&2
    port_ok=0
    return 1
}
set_port_attr addr_traddr "$IP"   || true
set_port_attr addr_trtype rdma    || true
set_port_attr addr_adrfam ipv4    || true
set_port_attr addr_trsvcid "$PORT" || true

# A bound port cannot be re-addressed, but this script can simply replace it: the
# only thing the port carries is the subsystem link, and that is re-created below.
# Without this, the requested address and the bound one can only be reconciled by a
# human running `rm`/`rmdir` by hand - which is exactly the state a previous run of
# this script left a peer in (port bound to one address, next run asking for another,
# `set -e` aborting on EACCES and printing a permissions error that names nothing).
if [ "$port_ok" = 0 ]; then
    echo "   rebinding: removing the bound port 1 and building it again"
    rm -f "$CONF/ports/1/subsystems"/* 2>/dev/null || true
    rmdir "$CONF/ports/1" 2>/dev/null || true
    mkdir -p "$CONF/ports/1"
    port_ok=1
    set_port_attr addr_traddr "$IP"    || true
    set_port_attr addr_trtype rdma     || true
    set_port_attr addr_adrfam ipv4     || true
    set_port_attr addr_trsvcid "$PORT" || true
fi

# The subsystem link is what BINDS the port, so it goes last, and only when the
# address is actually right: linking a port whose address is wrong would bind it to
# the wrong address and make the next run's fix need the unbind dance above.
#
# The link is skipped when it already exists for the same reason as the allowed_hosts
# one above: configfs cannot replace a link, so `ln -sfn` fails with EEXIST on every
# re-run.
port_link="$CONF/ports/1/subsystems/$SUBNQN"
if [ "$port_ok" = 1 ]; then
    if [ -L "$port_link" ]; then
        echo "   subsystem already linked to port 1"
    else
        ln -sfn "$CONF/subsystems/$SUBNQN" "$port_link"
    fi
else
    echo "   not linking the subsystem: the port address is wrong" >&2
fi

echo
echo "== ready.  What a host will see:"
cat "$CONF/subsystems/$SUBNQN/namespaces/1/device_path" | sed 's/^/   device_path: /'
echo "   subnqn:  $SUBNQN"
echo "   address: $IP:$PORT (rdma, ipv4)"
if [ -n "$AUTH_KEY" ]; then
    echo "   auth:    DH-HMAC-CHAP required (host NQN $AUTH_HOST_NQN, hash $AUTH_HASH${AUTH_DHGROUP:+, dhgroup $AUTH_DHGROUP})"
else
    echo "   auth:    none (allow_any_host=1)"
fi
if [ -n "${RXE_NETDEV:-}" ]; then
    echo "   transport: software RoCE (rxe0 on $RXE_NETDEV)"
fi
echo
echo "On Windows:"
echo "   .\\run_f5.ps1 -initiatorOnly -subnqn $SUBNQN \\"
echo "                 -serverIp $IP -port $PORT"
echo
echo "Before that, once, on Windows (RoCEv2 is UDP 4791 - an inbound block here"
echo "looks exactly like a target that never answers):"
echo "   New-NetFirewallRule -DisplayName 'NVMe-oF RoCEv2' -Direction Inbound \\"
echo "       -Protocol UDP -LocalPort 4791 -Action Allow"
echo
echo "Sanity check from this side, before Windows is involved:"
echo "   ping $IP            # from Windows, must succeed: same L2, no gateway"
echo "Watch this side with:  dmesg -w"
echo "Tear down with:        ./nvmet_teardown.sh $SUBNQN"
