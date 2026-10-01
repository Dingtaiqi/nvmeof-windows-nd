// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
//
//  Hardware-free self-test entry point: the DH-HMAC-CHAP primitives and protocol
//  pieces, with no NIC, no NetworkDirect SDK and no peer.
//
//  Why this exists as its own translation unit rather than a flag on f5_interop:
//  f5_interop.cpp includes nvmeof_rdma.h, which needs the NetworkDirect SDK headers
//  that are not part of this repository - so the only place the crypto self-test
//  could run was a machine that already had the stack set up.  nvmeof_auth.h and
//  nvmeof_dhchap.h need bcrypt and wincrypt and nothing else, so this file builds
//  anywhere MSVC does, which is what lets CI run it (see .github/workflows/ci.yml).
//
//  The self-tests themselves are the same ones `f5_interop -authselftest` calls;
//  this is a second door to the same room, opened where CI can reach it.
#include <stdio.h>
#include <stdint.h>

#include "nvmeof_auth.h"
#include "nvmeof_dhchap.h"

int main() {
    printf("=== DH-HMAC-CHAP primitives, against published vectors\n");
    int failures = nvmeof_auth::selfTest();
    printf("=== DH-HMAC-CHAP protocol pieces (target half)\n");
    nvmeof_dhchap::selfTest(failures);
    printf("\nauth self-test failures: %d\n", failures);
    return failures;
}
