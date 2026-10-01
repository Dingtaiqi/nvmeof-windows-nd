IQN="iqn.2024-01.local.rdma:linuxtarget"
CFG=/sys/kernel/config/target
echo 0 > "$CFG/iscsi/$IQN/tpgt_1/enable" 2>/dev/null || true
sleep 1
mkdir -p "$CFG/iscsi/$IQN/tpgt_1/np/192.168.1.9" && echo "np 192.168.1.9 ok" || echo "np 192.168.1.9 failed"
echo 1 > "$CFG/iscsi/$IQN/tpgt_1/enable"
sleep 2
echo "--- listeners:"; (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep 3260 || echo "  none on 3260"
echo "--- dmesg:"; dmesg | tail -5
echo "--- tcpdump: $(command -v tcpdump || echo MISSING)"
