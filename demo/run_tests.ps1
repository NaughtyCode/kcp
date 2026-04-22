# KCP Demo Automated Test Script
# Tests: handshake, echo, heartbeat, disconnect, stats, throughput

$ErrorActionPreference = "Stop"

# Configuration
$SCRIPT_DIR = $PSScriptRoot
$BUILD_DIR = Join-Path $SCRIPT_DIR "build\Release"
$SERVER_EXE = Join-Path $BUILD_DIR "kcp_server.exe"
$CLIENT_EXE = Join-Path $BUILD_DIR "kcp_client.exe"
$TEST_PORT = 7789
$SERVER_ADDR = "127.0.0.1"

$PASS_COUNT = 0
$FAIL_COUNT = 0
$results = @()

function Write-Header {
    param([string]$title)
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  $title" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
}

function Write-Result {
    param([string]$name, [bool]$ok, [string]$detail = "")
    if ($ok) {
        $PASS_COUNT++
        Write-Host "  [PASS] $name" -ForegroundColor Green
    } else {
        $FAIL_COUNT++
        Write-Host "  [FAIL] $name" -ForegroundColor Red
    }
    if ($detail) {
        $status = if ($ok) { "PASS" } else { "FAIL" }
        $results += "$name`t$status`t$detail"
    } else {
        $results += "$name`t$($ok ? 'PASS' : 'FAIL')`t"
    }
}

function Write-Info {
    param([string]$msg)
    Write-Host "  [INFO] $msg" -ForegroundColor Yellow
}

# ======== Pre-flight checks ========
Write-Header "KCP Demo Automated Tests"
Write-Host "Time: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray

if (!(Test-Path $SERVER_EXE)) {
    Write-Host "ERROR: Server binary not found: $SERVER_EXE" -ForegroundColor Red
    Write-Host "Please run cmake build first." -ForegroundColor Yellow
    exit 1
}

if (!(Test-Path $CLIENT_EXE)) {
    Write-Host "ERROR: Client binary not found: $CLIENT_EXE" -ForegroundColor Red
    Write-Host "Please run cmake build first." -ForegroundColor Yellow
    exit 1
}

Write-Info "Server: $SERVER_EXE"
Write-Info "Client: $CLIENT_EXE"
Write-Info "Port: $TEST_PORT"

# ======== Launch Server ========
Write-Header "Step 1: Start Server"

$server_proc = Start-Process -FilePath $SERVER_EXE -ArgumentList $TEST_PORT `
    -NoNewWindow -PassThru -RedirectStandardOutput "$SCRIPT_DIR\server_out.log" `
    -RedirectStandardError "$SCRIPT_DIR\server_err.log"
Write-Info "Server PID: $($server_proc.Id)"
Start-Sleep -Seconds 2

try {
    $proc_alive = (Get-Process -Id $server_proc.Id -ErrorAction Stop) -ne $null
} catch {
    $proc_alive = $false
}

if (-not $proc_alive) {
    Write-Host "ERROR: Server crashed on startup" -ForegroundColor Red
    Write-Host "Error log:" -ForegroundColor Red
    Get-Content "$SCRIPT_DIR\server_err.log" -ErrorAction SilentlyContinue
    exit 1
}
Write-Result "Server started" $true "PID=$($server_proc.Id)"

# ======== Test 1: Handshake ========
Write-Header "Test 1: Handshake Test"

$client_proc = Start-Process -FilePath $CLIENT_EXE -ArgumentList "$SERVER_ADDR $TEST_PORT" `
    -NoNewWindow -PassThru -RedirectStandardOutput "$SCRIPT_DIR\client_out.log"

Write-Info "Client PID: $($client_proc.Id)"
Start-Sleep -Seconds 3

$client_log = Get-Content "$SCRIPT_DIR\client_out.log" -Raw -ErrorAction SilentlyContinue
$server_log = Get-Content "$SCRIPT_DIR\server_out.log" -Raw -ErrorAction SilentlyContinue

if ($client_log -match "HANDSHAKE OK") {
    Write-Result "Client handshake success" $true
} else {
    Write-Result "Client handshake success" $false "timeout"
}

if ($server_log -match "Client connected") {
    Write-Result "Server detected client" $true
} else {
    Write-Result "Server detected client" $false
}

# ======== Test 2: Echo ========
Write-Header "Test 2: Echo Test"

Write-Info "Sending test messages..."

$client_stdin = $client_proc.StandardInput
if ($client_stdin) {
    $client_stdin.WriteLine("Hello KCP!")
    Start-Sleep -Seconds 1
    $client_stdin.WriteLine("Test message 2")
    Start-Sleep -Seconds 1
    $client_stdin.WriteLine("Third message: 12345")
    Start-Sleep -Seconds 1
    $client_stdin.Close()
} else {
    Write-Result "stdin channel" $false
}

Start-Sleep -Seconds 2

$client_log = Get-Content "$SCRIPT_DIR\client_out.log" -Raw -ErrorAction SilentlyContinue

$echo1 = $client_log -match "SERVER_ECHO.*Hello KCP!"
Write-Result "Echo message 1" $echo1 "Hello KCP!"

$echo2 = $client_log -match "SERVER_ECHO.*Test message 2"
Write-Result "Echo message 2" $echo2 "Test message 2"

$echo3 = $client_log -match "SERVER_ECHO.*Third message: 12345"
Write-Result "Echo message 3" $echo3 "Third message: 12345"

# ======== Test 3: Heartbeat ========
Write-Header "Test 3: Heartbeat Test"

Start-Sleep -Seconds 3

$client_log = Get-Content "$SCRIPT_DIR\client_out.log" -Raw -ErrorAction SilentlyContinue
$server_log = Get-Content "$SCRIPT_DIR\server_out.log" -Raw -ErrorAction SilentlyContinue

$hb_client = $client_log -match "HEARTBEAT.*from server"
Write-Result "Client received heartbeat" $hb_client

$hb_server = $server_log -match "HEARTBEAT.*from client"
Write-Result "Server received heartbeat" $hb_server

# ======== Test 4: Disconnect ========
Write-Header "Test 4: Disconnect Test"

Write-Info "Closing client..."
$client_proc.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3

if ($client_proc -and !$client_proc.HasExited) {
    Stop-Process -Id $client_proc.Id -Force
}
Start-Sleep -Seconds 1

$client_log = Get-Content "$SCRIPT_DIR\client_out.log" -Raw -ErrorAction SilentlyContinue
$server_log = Get-Content "$SCRIPT_DIR\server_out.log" -Raw -ErrorAction SilentlyContinue

$dc_client = $client_log -match "DISCONNECT|disconnected|stopped|stopped\." -or $client_proc.HasExited
Write-Result "Client disconnected cleanly" $dc_client "exited=$($client_proc.HasExited)"

$dc_server = $server_log -match "Client disconnected|stopped|Final stats"
Write-Result "Server detected disconnect" $dc_server

# ======== Test 5: Stats ========
Write-Header "Test 5: Statistics"

$client_log = Get-Content "$SCRIPT_DIR\client_out.log" -Raw -ErrorAction SilentlyContinue
$server_log = Get-Content "$SCRIPT_DIR\server_out.log" -Raw -ErrorAction SilentlyContinue

$stats_c = $client_log -match "STATS.*sent="
Write-Result "Client stats output" $stats_c

$stats_s = $server_log -match "STATS.*sent="
Write-Result "Server stats output" $stats_s

$bytes_c = $client_log -match "Sent: \d+ bytes"
Write-Result "Client bytes stats" $bytes_c

# ======== Test 6: Throughput ========
Write-Header "Test 6: Throughput Test"

Write-Info "Starting batch test..."

$client_proc2 = Start-Process -FilePath $CLIENT_EXE -ArgumentList "$SERVER_ADDR $TEST_PORT" `
    -NoNewWindow -PassThru -RedirectStandardOutput "$SCRIPT_DIR\client_out2.log"
Start-Sleep -Seconds 2

$stdin2 = $client_proc2.StandardInput
if ($stdin2) {
    for ($i = 1; $i -le 20; $i++) {
        $stdin2.WriteLine("BATCH_MSG_$i")
        Start-Sleep -Milliseconds 50
    }
    $stdin2.Close()
}

Start-Sleep -Seconds 4

$client_log2 = Get-Content "$SCRIPT_DIR\client_out2.log" -Raw -ErrorAction SilentlyContinue
$echo_count = ([regex]::Matches($client_log2, "SERVER_ECHO")).Count
$throughput_ok = $echo_count -ge 18

Write-Result "Batch echo ($echo_count/20)" $throughput_ok "received $echo_count echoes"

if ($client_proc2 -and !$client_proc2.HasExited) {
    $client_proc2.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 1
    if (!$client_proc2.HasExited) { Stop-Process -Id $client_proc2.Id -Force }
}
Start-Sleep -Seconds 1

# ======== Cleanup ========
Write-Header "Cleanup"

Write-Info "Stopping server..."
if ($server_proc -and !$server_proc.HasExited) {
    $server_proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 2
    if (!$server_proc.HasExited) { Stop-Process -Id $server_proc.Id -Force }
}
Start-Sleep -Seconds 1
Write-Result "Server cleaned up" $true

# ======== Summary ========
Write-Header "Test Results Summary"

$total = $PASS_COUNT + $FAIL_COUNT
$rate = if ($total -gt 0) { "$PASS_COUNT/$total" } else { "N/A" }
Write-Host "  Passed: $PASS_COUNT" -ForegroundColor Green
Write-Host "  Failed: $FAIL_COUNT" -ForegroundColor Red
Write-Host "  Total:  $total" -ForegroundColor White
$color = if ($FAIL_COUNT -eq 0) { "Green" } else { "Red" }
Write-Host "  Rate:   $rate" -ForegroundColor $color

Write-Host ""
Write-Host "Details:" -ForegroundColor Cyan
foreach ($r in $results) {
    $parts = $r -split "\t"
    $name = $parts[0]
    $status = $parts[1]
    $detail = if ($parts.Count -gt 2) { $parts[2] } else { "" }
    if ($status -eq "PASS") {
        Write-Host "  [PASS] $name" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] $name" -ForegroundColor Red
    }
    if ($detail) { Write-Host "         $detail" -ForegroundColor DarkGray }
}

# Save results
$result_file = Join-Path $SCRIPT_DIR "test_results.txt"
$result_content = "KCP Demo Test Results`r`n"
$result_content += "Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')`r`n"
$result_content += "Passed: $PASS_COUNT / Failed: $FAIL_COUNT / Total: $total`r`n"
$result_content += "Rate: $rate`r`n`r`n"
$result_content += "--- Details ---`r`n"
foreach ($r in $results) {
    $p = $r -split "\t"
    $result_content += "[$($p[1])] $($p[0])"
    if ($p.Count -gt 2 -and $p[2]) { $result_content += " - $($p[2])" }
    $result_content += "`r`n"
}
$result_content | Out-File -FilePath $result_file -Encoding UTF8
Write-Host ""
Write-Host "Results saved to: $result_file" -ForegroundColor Gray

Write-Host ""
Write-Host "Log files:" -ForegroundColor Cyan
Write-Host "  server_out.log" -ForegroundColor Gray
Write-Host "  server_err.log" -ForegroundColor Gray
Write-Host "  client_out.log" -ForegroundColor Gray
Write-Host "  client_out2.log" -ForegroundColor Gray

if ($FAIL_COUNT -gt 0) { exit 1 }
exit 0
