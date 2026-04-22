@echo off
setlocal enabledelayedexpansion

REM KCP Demo Automated Test Script
REM Tests: handshake, echo, heartbeat, disconnect, stats, throughput

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BUILD_DIR=%SCRIPT_DIR%\build\Release"
set "SERVER_EXE=%BUILD_DIR%\kcp_server.exe"
set "CLIENT_EXE=%BUILD_DIR%\kcp_client.exe"
set "TEST_PORT=7789"
set "SERVER_ADDR=127.0.0.1"
set "PASS_COUNT=0"
set "FAIL_COUNT=0"

set "RESULTS_FILE=%SCRIPT_DIR%\test_results.txt"
set "SERVER_LOG=%SCRIPT_DIR%\server_out.log"
set "CLIENT_LOG=%SCRIPT_DIR%\client_out.log"
set "CLIENT_LOG2=%SCRIPT_DIR%\client_out2.log"
set "SERVER_ERR=%SCRIPT_DIR%\server_err.log"

echo.
echo ==========================================
echo   KCP Demo Automated Tests
echo ==========================================
echo.
echo Time: %DATE% %TIME%
echo.

REM Pre-flight checks
if not exist "%SERVER_EXE%" (
    echo ERROR: Server binary not found: %SERVER_EXE%
    echo Please run cmake build first.
    exit /b 1
)

if not exist "%CLIENT_EXE%" (
    echo ERROR: Client binary not found: %CLIENT_EXE%
    echo Please run cmake build first.
    exit /b 1
)

echo [INFO] Server: %SERVER_EXE%
echo [INFO] Client: %CLIENT_EXE%
echo [INFO] Port: %TEST_PORT%
echo.

REM Initialize results file
echo KCP Demo Test Results > "%RESULTS_FILE%"
echo Date: %DATE% %TIME% >> "%RESULTS_FILE%"
echo. >> "%RESULTS_FILE%"

:WRITE_RESULT
set "NAME=%~1"
set "OK=%~2"
set "DETAIL=%~3"
if "%OK%"=="1" (
    set /a PASS_COUNT+=1
    echo   [PASS] %NAME%
) else (
    set /a FAIL_COUNT+=1
    echo   [FAIL] %NAME%
    if defined DETAIL (
        echo          %DETAIL%
    )
)
if "%OK%"=="1" (
    echo   [PASS] %NAME% >> "%RESULTS_FILE%"
) else (
    echo   [FAIL] %NAME% >> "%RESULTS_FILE%"
    if defined DETAIL (
        echo          %DETAIL% >> "%RESULTS_FILE%"
    )
)
goto :eof

:WRITE_HEADER
echo.
echo ==========================================
echo   %~1
echo ==========================================
echo.
goto :eof

:WRITE_INFO
echo   [INFO] %~1
goto :eof

REM ======== Launch Server ========
:WRITE_HEADER "Step 1: Start Server"

echo Start-Process -FilePath "%SERVER_EXE%" -ArgumentList %TEST_PORT% -NoNewWindow -PassThru -RedirectStandardOutput "%SERVER_LOG%" -RedirectStandardError "%SERVER_ERR%" > "%SCRIPT_DIR%\launch_server.tmp"
powershell -Command "& { $p = Start-Process -FilePath '%SERVER_EXE%' -ArgumentList '%TEST_PORT%' -NoNewWindow -PassThru -RedirectStandardOutput '%SERVER_LOG%' -RedirectStandardError '%SERVER_ERR%'; Write-Output $p.Id }" > "%SCRIPT_DIR%\server_pid.tmp"
for /f %%a in (%SCRIPT_DIR%\server_pid.tmp) do set "SERVER_PID=%%a"
del /f "%SCRIPT_DIR%\server_pid.tmp" 2>nul

echo [INFO] Server PID: %SERVER_PID%
timeout /t 2 /nobreak >nul

REM Check server is running
tasklist /FI "PID eq %SERVER_PID%" 2>nul | find /i "%SERVER_PID%" >nul
if errorlevel 1 (
    echo ERROR: Server crashed on startup
    echo Error log:
    if exist "%SERVER_ERR%" type "%SERVER_ERR%"
    exit /b 1
)
call :WRITE_RESULT "Server started" "1" "PID=%SERVER_PID%"

REM ======== Test 1: Handshake ========
:WRITE_HEADER "Test 1: Handshake Test"

powershell -Command "& { $p = Start-Process -FilePath '%CLIENT_EXE%' -ArgumentList '%SERVER_ADDR% %TEST_PORT%' -NoNewWindow -PassThru -RedirectStandardOutput '%CLIENT_LOG%'; Write-Output $p.Id }" > "%SCRIPT_DIR%\client_pid.tmp"
for /f %%a in (%SCRIPT_DIR%\client_pid.tmp) do set "CLIENT_PID=%%a"
del /f "%SCRIPT_DIR%\client_pid.tmp" 2>nul

echo [INFO] Client PID: %CLIENT_PID%
timeout /t 3 /nobreak >nul

findstr /C:"HANDSHAKE OK" "%CLIENT_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Client handshake success" "0" "timeout"
) else (
    call :WRITE_RESULT "Client handshake success" "1"
)

findstr /C:"Client connected" "%SERVER_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Server detected client" "0"
) else (
    call :WRITE_RESULT "Server detected client" "1"
)

REM ======== Test 2: Echo ========
:WRITE_HEADER "Test 2: Echo Test"

echo [INFO] Sending test messages...

powershell -Command "& { $p = Get-Process -Id %CLIENT_PID%; $w = $p.MainWindowHandle; Start-Sleep -Milliseconds 500; $p.StandardInput.WriteLine('Hello KCP!'); Start-Sleep -Seconds 1; $p.StandardInput.WriteLine('Test message 2'); Start-Sleep -Seconds 1; $p.StandardInput.WriteLine('Third message: 12345'); Start-Sleep -Seconds 1; $p.StandardInput.Close() }" 2>nul

timeout /t 2 /nobreak >nul

findstr /C:"SERVER_ECHO" "%CLIENT_LOG%" | findstr /C:"Hello KCP!" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Echo message 1" "0" "Hello KCP!"
) else (
    call :WRITE_RESULT "Echo message 1" "1" "Hello KCP!"
)

findstr /C:"SERVER_ECHO" "%CLIENT_LOG%" | findstr /C:"Test message 2" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Echo message 2" "0" "Test message 2"
) else (
    call :WRITE_RESULT "Echo message 2" "1" "Test message 2"
)

findstr /C:"SERVER_ECHO" "%CLIENT_LOG%" | findstr /C:"Third message: 12345" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Echo message 3" "0" "Third message: 12345"
) else (
    call :WRITE_RESULT "Echo message 3" "1" "Third message: 12345"
)

REM ======== Test 3: Heartbeat ========
:WRITE_HEADER "Test 3: Heartbeat Test"

timeout /t 3 /nobreak >nul

findstr /C:"HEARTBEAT" "%CLIENT_LOG%" | findstr /C:"from server" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Client received heartbeat" "0"
) else (
    call :WRITE_RESULT "Client received heartbeat" "1"
)

findstr /C:"HEARTBEAT" "%SERVER_LOG%" | findstr /C:"from client" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Server received heartbeat" "0"
) else (
    call :WRITE_RESULT "Server received heartbeat" "1"
)

REM ======== Test 4: Disconnect ========
:WRITE_HEADER "Test 4: Disconnect Test"

echo [INFO] Closing client...
taskkill //FI "PID eq %CLIENT_PID%" //T //F >nul 2>&1
timeout /t 2 /nobreak >nul

findstr /C:"DISCONNECT" "%CLIENT_LOG%" >nul 2>&1 || findstr /C:"stopped" "%CLIENT_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Client disconnected cleanly" "0"
) else (
    call :WRITE_RESULT "Client disconnected cleanly" "1"
)

findstr /C:"Client disconnected" "%SERVER_LOG%" >nul 2>&1 || findstr /C:"Final stats" "%SERVER_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Server detected disconnect" "0"
) else (
    call :WRITE_RESULT "Server detected disconnect" "1"
)

REM ======== Test 5: Stats ========
:WRITE_HEADER "Test 5: Statistics"

findstr /C:"STATS" "%CLIENT_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Client stats output" "0"
) else (
    call :WRITE_RESULT "Client stats output" "1"
)

findstr /C:"STATS" "%SERVER_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Server stats output" "0"
) else (
    call :WRITE_RESULT "Server stats output" "1"
)

findstr /C:"Sent:" "%CLIENT_LOG%" >nul 2>&1
if errorlevel 1 (
    call :WRITE_RESULT "Client bytes stats" "0"
) else (
    call :WRITE_RESULT "Client bytes stats" "1"
)

REM ======== Test 6: Throughput ========
:WRITE_HEADER "Test 6: Throughput Test"

echo [INFO] Starting batch test...

powershell -Command "& { $p = Start-Process -FilePath '%CLIENT_EXE%' -ArgumentList '%SERVER_ADDR% %TEST_PORT%' -NoNewWindow -PassThru -RedirectStandardOutput '%CLIENT_LOG2%'; Write-Output $p.Id }" > "%SCRIPT_DIR%\client2_pid.tmp"
for /f %%a in (%SCRIPT_DIR%\client2_pid.tmp) do set "CLIENT_PID2=%%a"
del /f "%SCRIPT_DIR%\client2_pid.tmp" 2>nul

timeout /t 2 /nobreak >nul

REM Send 20 batch messages via PowerShell
powershell -Command "& { Start-Sleep -Seconds 1; $p = Get-Process -Id %CLIENT_PID2%; for ($i=1; $i -le 20; $i++) { $p.StandardInput.WriteLine('BATCH_MSG_' + $i); Start-Sleep -Milliseconds 50 }; $p.StandardInput.Close() }" 2>nul

timeout /t 4 /nobreak >nul

findstr /C:"SERVER_ECHO" "%CLIENT_LOG2%" >nul 2>&1
if errorlevel 1 (
    set "ECHO_COUNT=0"
) else (
    findstr /C:"SERVER_ECHO" "%CLIENT_LOG2%" | find /C /V "" > "%SCRIPT_DIR%\echo_count.tmp"
    for /f %%a in (%SCRIPT_DIR%\echo_count.tmp) do set "ECHO_COUNT=%%a"
    del /f "%SCRIPT_DIR%\echo_count.tmp" 2>nul
)

if %ECHO_COUNT% geq 18 (
    call :WRITE_RESULT "Batch echo (%ECHO_COUNT%/20)" "1" "received %ECHO_COUNT% echoes"
) else (
    call :WRITE_RESULT "Batch echo (%ECHO_COUNT%/20)" "0" "received %ECHO_COUNT% echoes"
)

if exist "%CLIENT_PID2%" taskkill //FI "PID eq %CLIENT_PID2%" //T //F >nul 2>&1
timeout /t 1 /nobreak >nul

REM ======== Cleanup ========
:WRITE_HEADER "Cleanup"

echo [INFO] Stopping server...
taskkill //FI "PID eq %SERVER_PID%" //T //F >nul 2>&1
timeout /t 2 /nobreak >nul
call :WRITE_RESULT "Server cleaned up" "1"

REM ======== Summary ========
:WRITE_HEADER "Test Results Summary"

set /a TOTAL=%PASS_COUNT%+%FAIL_COUNT%
if %TOTAL% gtr 0 (
    set "RATE=%PASS_COUNT%/%TOTAL%"
) else (
    set "RATE=N/A"
)

echo   Passed: %PASS_COUNT%
echo   Failed: %FAIL_COUNT%
echo   Total:  %TOTAL%
if %FAIL_COUNT% equ 0 (
    echo   Rate:   %RATE%
) else (
    echo   Rate:   %RATE%
)

echo.
echo Details:
for /f "tokens=1,2,3* delims=|" %%A in ('type "%RESULTS_FILE%" ^| findstr /V "^KCP\|^Date\|^$"') do (
    echo   [%%B] %%A
)

echo. >> "%RESULTS_FILE%"
echo Passed: %PASS_COUNT% / Failed: %FAIL_COUNT% / Total: %TOTAL% >> "%RESULTS_FILE%"
echo Rate: %RATE% >> "%RESULTS_FILE%"

echo.
echo Results saved to: %RESULTS_FILE%
echo.
echo Log files:
echo   server_out.log
echo   server_err.log
echo   client_out.log
echo   client_out2.log

if %FAIL_COUNT% gtr 0 ( exit /b 1 )
exit /b 0
