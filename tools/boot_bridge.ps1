$ErrorActionPreference = 'Continue'
Start-Sleep -Seconds 5
if (-not (Get-NetTCPConnection -LocalPort 3260 -State Listen -EA SilentlyContinue)) {
  Start-Process -FilePath 'D:\rdma\nvmeof\src\iscsi_tcp_bridge.exe' -ArgumentList @('-listen','127.0.0.1','3260','-target','127.0.0.1','4421','-lun','0','32768') -RedirectStandardOutput 'D:\nvmeof\bootbrg.log' -RedirectStandardError 'D:\nvmeof\bootbrg.err' -WindowStyle Hidden
}