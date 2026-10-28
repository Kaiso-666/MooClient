# Reports the MooClient process tree memory (host + WebView2 children).
param(
    [switch]$Detailed
)

$hostProc = Get-Process MooClient -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $hostProc) { throw "MooClient is not running." }

$all = Get-CimInstance Win32_Process
$byParent = @{}
foreach ($p in $all) {
    if (-not $byParent.ContainsKey([int]$p.ParentProcessId)) {
        $byParent[[int]$p.ParentProcessId] = New-Object System.Collections.Generic.List[object]
    }
    $byParent[[int]$p.ParentProcessId].Add($p)
}

$tree = New-Object System.Collections.Generic.List[object]
$queue = New-Object System.Collections.Generic.Queue[int]
$queue.Enqueue($hostProc.Id)
while ($queue.Count -gt 0) {
    $id = $queue.Dequeue()
    $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $proc) { continue }
    $tree.Add([pscustomobject]@{
        Id      = $id
        Name    = $proc.ProcessName
        Working = $proc.WorkingSet64
    })
    if ($byParent.ContainsKey($id)) {
        foreach ($child in $byParent[$id]) { $queue.Enqueue([int]$child.ProcessId) }
    }
}

$total = ($tree | Measure-Object Working -Sum).Sum
"total_mb=$([math]::Round($total / 1MB, 1))"
"processes=$($tree.Count)"
if ($Detailed) {
    $tree | Sort-Object Working -Descending |
        ForEach-Object { "{0,7} {1,-18} {2,8:N1} MB" -f $_.Id, $_.Name, ($_.Working / 1MB) }
}
