#!/bin/sh
# Rebuild the peer's nvmet target with DH-HMAC-CHAP required, at the RoCE address.
#
# Runs as an ordinary user: every root action below goes through the peer's NOPASSWD
# sudoers rules (teardown / setup / rdma).  A file rather than a one-liner because a
# command string travelling PowerShell -> ssh.exe -> bash loses its quotes.
set -e
NQN=nqn.2024-01.local.rdma:linux-nvmet
KEYFILE="$HOME/.f5_nvmet_auth.key"

echo "=== teardown (the fixed revision: removes allowed_hosts, then verifies)"
sudo -n "$HOME/nvmeof/linux/nvmet_teardown.sh" "$NQN"
echo "=== the RoCE address and rxe0 (a reboot drops both - 'ip addr add' is not persistent)"
# After a reboot enp55s0 has only its LAN address: the RoCE one was added at run time.
sudo -n /usr/bin/ip addr add 192.168.100.5/24 dev enp55s0 2>/dev/null || true
ip -brief addr show enp55s0 | sed 's/^/   /'
echo "=== rxe0"
sudo -n /usr/bin/rdma link add rxe0 type rxe netdev enp55s0 2>/dev/null || true
rdma link show | sed 's/^/   /'
echo "=== setup with authentication at 192.168.100.5"
sudo -n "$HOME/nvmeof/linux/nvmet_setup.sh" 192.168.100.5 "$NQN" 4420 64 "$KEYFILE"
echo "=== read-back"
echo -n "   addr_traddr     = "; cat /sys/kernel/config/nvmet/ports/1/addr_traddr
echo -n "   allow_any_host  = "; cat "/sys/kernel/config/nvmet/subsystems/$NQN/attr_allow_any_host"
echo -n "   allowed_hosts   = "; ls "/sys/kernel/config/nvmet/subsystems/$NQN/allowed_hosts/" 2>&1
echo -n "   dhchap_key set  = "; wc -c < "/sys/kernel/config/nvmet/hosts/nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001/dhchap_key" 2>&1
echo -n "   dhgroup         = "; cat "/sys/kernel/config/nvmet/hosts/nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001/dhchap_dhgroup" 2>&1
echo "=== a second setup run (this is what used to die on EINVAL)"
sudo -n "$HOME/nvmeof/linux/nvmet_setup.sh" 192.168.100.5 "$NQN" 4420 64 "$KEYFILE" >/dev/null 2>&1 \
    && echo "   re-run: rc=0 (idempotent)" || echo "   re-run: rc=$? (NOT idempotent)"
