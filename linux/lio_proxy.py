#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  lio_proxy.py - a byte-for-byte tap in front of the reference iSCSI target.
#
#  The peer has no tcpdump, and this is better anyway: the proxy relays a real
#  Windows initiator to LIO (the in-kernel reference target) and writes every byte
#  in both directions to a log with direction markers.  What LIO answers is the
#  ground truth for the question this project has been bisecting against Windows:
#  what a target must send at the NORMAL session's security stage.
#
#  Usage: python3 lio_proxy.py [listen-port] [target-port] [logfile]
# ===========================================================================
import socket, select, sys, threading, time

LISTEN = sys.argv[1] if len(sys.argv) > 1 else "0.0.0.0:3261"
TARGET_HOST = sys.argv[2].split(':')[0] if len(sys.argv) > 2 else '127.0.0.1'
TARGET      = int(sys.argv[2].split(':')[-1]) if len(sys.argv) > 2 else 3260
LOG    = sys.argv[3] if len(sys.argv) > 3 else '/tmp/lio_proxy.log'
PATCH_FROM = sys.argv[4].encode() if len(sys.argv) > 4 else b''
PATCH_TO   = sys.argv[5].encode() if len(sys.argv) > 5 else b''

logf = open(LOG, 'w', buffering=1)
lock = threading.Lock()

def emit(tag, data):
    with lock:
        logf.write("### %s  %d bytes\n" % (tag, len(data)))
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hexs = ' '.join('%02x' % b for b in chunk)
            asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
            logf.write("  %04d: %-47s  %s\n" % (i, hexs, asc))

def pump(src, dst, tag, patch=None):
    try:
        while True:
            r, _, _ = select.select([src], [], [], 30)
            if not r:
                continue
            data = src.recv(65536)
            if not data:
                break
            if patch:
                # Rewrite the advertised TargetAddress in the SendTargets reply so it
                # points back at THIS proxy instead of at the real target.  Without it
                # the initiator opens the normal session straight to the target's own
                # address and the interesting half of the handshake is never seen.
                # The two strings are deliberately the same length (same host, port
                # 3261 -> 3260), so no length field has to be recomputed.
                data = data.replace(patch[0], patch[1])
            emit(tag, data)
            dst.sendall(data)
    except Exception as e:
        with lock:
            logf.write("### %s closed: %s\n" % (tag, e))
    finally:
        try: dst.shutdown(socket.SHUT_WR)
        except Exception: pass

def handle(client, addr):
    with lock:
        logf.write("\n================ connection from %s:%d ================\n" % addr)
    up = socket.create_connection((TARGET_HOST, TARGET))
    t1 = threading.Thread(target=pump, args=(client, up, 'INITIATOR -> LIO'))
    t2 = threading.Thread(target=pump, args=(up, client, 'LIO -> INITIATOR',
                                             (PATCH_FROM, PATCH_TO)))
    t1.start(); t2.start(); t1.join(); t2.join()
    up.close(); client.close()

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
host, _, port = LISTEN.partition(':'); srv.bind((host or '0.0.0.0', int(port or LISTEN)))
srv.listen(4)
print("lio_proxy: %s -> %s:%d, log %s" % (LISTEN, TARGET_HOST, TARGET, LOG))
sys.stdout.flush()
while True:
    c, a = srv.accept()
    threading.Thread(target=handle, args=(c, a), daemon=True).start()




