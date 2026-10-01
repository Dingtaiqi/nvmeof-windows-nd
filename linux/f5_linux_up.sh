#!/bin/sh
# ===========================================================================
#  f5_linux_up.sh - bring the Linux peer up in one command.
#
#  Does, in order, with a check after each step:
#    1. load rdma_rxe and create rxe0 on the given interface
#    2. add a secondary address in the RoCE subnet to that interface
#       (rxe resolves the peer by ARP, so both ends must be in one subnet)
#    3. ping the Windows RoCE endpoint - the decisive test of the whole setup
#    4. configure nvmet over RDMA (nvmet_setup.sh)
#
#  Usage, as root, on the Linux peer:
#      ./f5_linux_up.sh <iface> [roce-ip] [windows-roce-ip]
#  Defaults:
#      roce-ip         192.168.100.5     (this peer, on the CX3 port's subnet)
#      windows-roce-ip 192.168.100.2     (this project's RoCE endpoint)
#
#  Test first without changing anything:
#      ./f5_linux_up.sh --check enp3s0
# ===========================================================================
set -e

CHECK_ONLY=0
if [ "$1" = "--check" ]; then CHECK_ONLY=1; shift; fi

IFACE="${1:?usage: f5_linux_up.sh [--check] <iface> [roce-ip] [windows-roce-ip]}"
ROCE_IP="${2:-192.168.100.5}"
WIN_IP="${3:-192.168.100.2}"
HERE="$(cd "$(dirname "$0")" && pwd)"

say()  { printf '%s\n' "$*"; }
ok()   { printf '  [ok] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*" >&2; }
die()  { bad "$*"; exit 1; }

if [ "$(id -u)" != "0" ] && [ "$CHECK_ONLY" = 0 ]; then
    die "must run as root (module load, rxe link, addresses, configfs)"
fi

say "== 0. interface $IFACE"
ip link show "$IFACE" >/dev/null 2>&1 || {
    bad "no such interface: $IFACE"; ip -brief link show >&2; exit 1; }
ip -brief addr show "$IFACE" | sed 's/^/   /'

say "== 1. prerequisites"
for m in nvmet nvmet-rdma rdma_rxe brd; do
    if modinfo "$m" >/dev/null 2>&1; then ok "module $m available"
    else bad "module $m NOT available in this kernel"; MISSING=1; fi
done
command -v rdma >/dev/null 2>&1 && ok "rdma tool present (iproute2)" \
    || bad "rdma tool missing - install iproute2"
[ "${MISSING:-0}" = 0 ] || die "a required kernel module is missing; a different kernel or a VM with a matching kernel is needed"

say "== 2. plan"
say "   rxe0 on $IFACE"
say "   add $ROCE_IP/24 to $IFACE   (secondary address; keeps the existing one)"
say "   expect $WIN_IP to answer ping (same L2, no gateway)"
if [ "$CHECK_ONLY" = 1 ]; then
    say ""
    say "--check only: nothing was changed.  Re-run without --check to apply."
    exit 0
fi

say "== 3. software RoCE"
modprobe rdma_rxe || die "modprobe rdma_rxe failed"
rdma link add rxe0 type rxe netdev "$IFACE" 2>/dev/null || true
rdma link show | sed 's/^/   /'
rdma link show 2>/dev/null | grep -q rxe || die "rxe0 is not up"

say "== 4. address in the RoCE subnet"
if ip -4 addr show "$IFACE" | grep -q "inet $ROCE_IP/"; then
    ok "$ROCE_IP already on $IFACE"
else
    ip addr add "$ROCE_IP/24" dev "$IFACE"
    ok "added $ROCE_IP/24 to $IFACE"
fi
ip -brief addr show "$IFACE" | sed 's/^/   /'

say "== 5. can we reach the Windows RoCE endpoint?"
if ping -c2 -W2 "$WIN_IP" >/dev/null 2>&1; then
    ok "ping $WIN_IP works - the two ends share an L2 segment"
else
    bad "ping $WIN_IP failed."
    cat >&2 <<'EOF'
     Things to check, in this order:
       1. Windows: is the bridge up and does '以太网 24' still hold 192.168.100.2?
       2. Windows firewall: an inbound block on UDP 4791 looks exactly like a
          target that never answers.  Allow it:
            New-NetFirewallRule -DisplayName 'NVMe-oF RoCEv2' -Direction Inbound `
                -Protocol UDP -LocalPort 4791 -Action Allow
       3. Is this interface really on the same router/switch as the Windows LAN NIC?
          rxe will not route: a gateway between the two cannot work.
EOF
    exit 1
fi

say "== 6. nvmet over RDMA"
RXE_NETDEV="$IFACE" "$HERE/nvmet_setup.sh" "$ROCE_IP"

say ""
say "== ready.  The Linux peer is '$ROCE_IP', our Windows endpoint is '$WIN_IP'."
say "   Windows side, direction A (our host -> this target):"
say "     .\\run_f5.ps1 -initiatorOnly -serverIp $ROCE_IP -port 4420"
say "   Direction B (Linux host -> our target):"
say "     nvme connect -t rdma -a $WIN_IP -s 4420 -n nqn.2024-01.local.rdma:windows-nd"
