# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = 'Stop'
Copy-Item 'D:\rdma\nvmeof\src\driver\sym\d2\nvmeofnd2.sys' 'C:\Windows\System32\drivers\nvmeofnd2.sys' -Force
'copied' | Out-File 'D:\rdma\copy_result.txt' -Encoding ASCII