param([int]$Bytes = 1048576)
$bytes = New-Object 'byte[]' $Bytes
"after New-Object: type=$($bytes.GetType().FullName) len=$($bytes.Length)"
$rng = New-Object System.Random 1
$rng.NextBytes($bytes)
"after NextBytes: type=$($bytes.GetType().FullName) len=$($bytes.Length)"
