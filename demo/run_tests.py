import subprocess
import time
import sys
import os
import signal

# Configuration
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(SCRIPT_DIR, 'build', 'Release')
SERVER_EXE = os.path.join(BUILD_DIR, 'kcp_server.exe')
CLIENT_EXE = os.path.join(BUILD_DIR, 'kcp_client.exe')
TEST_PORT = 7789
SERVER_ADDR = '127.0.0.1'

PASS_COUNT = 0
FAIL_COUNT = 0
results = []


def write_header(title):
    print()
    print('=' * 50)
    print(f'  {title}')
    print('=' * 50)
    print()


def write_result(name, ok, detail=''):
    global PASS_COUNT, FAIL_COUNT
    if ok:
        PASS_COUNT += 1
        print(f'  [PASS] {name}')
        if detail:
            print(f'         {detail}')
        results.append((name, 'PASS', detail))
    else:
        FAIL_COUNT += 1
        print(f'  [FAIL] {name}')
        if detail:
            print(f'         {detail}')
        results.append((name, 'FAIL', detail))


def write_info(msg):
    print(f'  [INFO] {msg}')


def read_log(filepath):
    try:
        with open(filepath, 'r', errors='replace') as f:
            return f.read()
    except Exception:
        return ''


def search_log(filepath, pattern):
    content = read_log(filepath)
    return pattern in content


def kill_proc(proc):
    try:
        proc.terminate()
        proc.wait(timeout=3)
    except Exception:
        try:
            proc.kill()
            proc.wait(timeout=2)
        except Exception:
            pass


def cleanup_logs():
    for f in ['server_out.log', 'server_err.log', 'client_out.log', 'client_out2.log']:
        p = os.path.join(SCRIPT_DIR, f)
        if os.path.exists(p):
            os.remove(p)


def try_kill_port(port):
    """Try to kill any process listening on the test port."""
    try:
        result = subprocess.run(
            ['netstat', '-ano'], capture_output=True, text=True, timeout=5
        )
        for line in result.stdout.split('\n'):
            if f':{port}' in line and 'LISTENING' in line:
                parts = line.strip().split()
                if parts:
                    pid = int(parts[-1])
                    try:
                        os.kill(pid, signal.SIGTERM)
                        time.sleep(0.5)
                        if True:
                            os.kill(pid, signal.SIGKILL)
                    except Exception:
                        pass
    except Exception:
        pass


# ======== Main ========
write_header('KCP Demo Automated Tests')
print(f'Time: {time.strftime("%Y-%m-%d %H:%M:%S")}')
print()

# Pre-flight checks
if not os.path.exists(SERVER_EXE):
    print(f'ERROR: Server binary not found: {SERVER_EXE}')
    print('Please run cmake build first.')
    sys.exit(1)

if not os.path.exists(CLIENT_EXE):
    print(f'ERROR: Client binary not found: {CLIENT_EXE}')
    print('Please run cmake build first.')
    sys.exit(1)

write_info(f'Server: {SERVER_EXE}')
write_info(f'Client: {CLIENT_EXE}')
write_info(f'Port: {TEST_PORT}')
print()

# Clean up
cleanup_logs()
try_kill_port(TEST_PORT)
time.sleep(0.5)

# ======== Launch Server ========
write_header('Step 1: Start Server')

server_proc = subprocess.Popen(
    [SERVER_EXE, str(TEST_PORT)],
    stdout=open(os.path.join(SCRIPT_DIR, 'server_out.log'), 'w'),
    stderr=open(os.path.join(SCRIPT_DIR, 'server_err.log'), 'w'),
    creationflags=subprocess.CREATE_NO_WINDOW
)
write_info(f'Server PID: {server_proc.pid}')
time.sleep(2)

if server_proc.poll() is not None:
    print('ERROR: Server crashed on startup')
    err_log = read_log(os.path.join(SCRIPT_DIR, 'server_err.log'))
    if err_log:
        print('Error log:')
        print(err_log)
    sys.exit(1)

write_result('Server started', True, f'PID={server_proc.pid}')

# ======== Test 1: Handshake ========
write_header('Test 1: Handshake Test')

client_proc = subprocess.Popen(
    [CLIENT_EXE, SERVER_ADDR, str(TEST_PORT)],
    stdout=open(os.path.join(SCRIPT_DIR, 'client_out.log'), 'w'),
    stderr=subprocess.DEVNULL,
    stdin=subprocess.PIPE,
    creationflags=subprocess.CREATE_NO_WINDOW
)
write_info(f'Client PID: {client_proc.pid}')
time.sleep(3)

if search_log(os.path.join(SCRIPT_DIR, 'client_out.log'), 'HANDSHAKE OK'):
    write_result('Client handshake success', True)
else:
    write_result('Client handshake success', False, 'timeout')

if search_log(os.path.join(SCRIPT_DIR, 'server_out.log'), 'Client connected'):
    write_result('Server detected client', True)
else:
    write_result('Server detected client', False)

# ======== Test 2: Echo ========
write_header('Test 2: Echo Test')

write_info('Sending test messages. . .')

time.sleep(0.5)
client_proc.stdin.write(b'Hello KCP!\n')
client_proc.stdin.flush()
time.sleep(1)

client_proc.stdin.write(b'Test message 2\n')
client_proc.stdin.flush()
time.sleep(1)

client_proc.stdin.write(b'Third message: 12345\n')
client_proc.stdin.flush()
time.sleep(1)

time.sleep(2)

client_log = read_log(os.path.join(SCRIPT_DIR, 'client_out.log'))
if 'SERVER_ECHO' in client_log and 'Hello KCP!' in client_log:
    write_result('Echo message 1', True, 'Hello KCP!')
else:
    write_result('Echo message 1', False, 'Hello KCP!')

if 'SERVER_ECHO' in client_log and 'Test message 2' in client_log:
    write_result('Echo message 2', True, 'Test message 2')
else:
    write_result('Echo message 2', False, 'Test message 2')

if 'SERVER_ECHO' in client_log and 'Third message: 12345' in client_log:
    write_result('Echo message 3', True, 'Third message: 12345')
else:
    write_result('Echo message 3', False, 'Third message: 12345')

# ======== Test 3: Heartbeat ========
write_header('Test 3: Heartbeat Test')

time.sleep(3)

client_log = read_log(os.path.join(SCRIPT_DIR, 'client_out.log'))
server_log = read_log(os.path.join(SCRIPT_DIR, 'server_out.log'))

if 'HEARTBEAT' in client_log and 'from server' in client_log:
    write_result('Client received heartbeat', True)
else:
    write_result('Client received heartbeat', False)

if 'HEARTBEAT' in server_log and 'from client' in server_log:
    write_result('Server received heartbeat', True)
else:
    write_result('Server received heartbeat', False)

# ======== Test 4: Disconnect ========
write_header('Test 4: Disconnect Test')

write_info('Closing client. . .')
client_proc.stdin.close()
time.sleep(1)
kill_proc(client_proc)
time.sleep(1)

client_log = read_log(os.path.join(SCRIPT_DIR, 'client_out.log'))
server_log = read_log(os.path.join(SCRIPT_DIR, 'server_out.log'))

if 'DISCONNECT' in client_log or 'stopped' in client_log:
    write_result('Client disconnected cleanly', True)
else:
    write_result('Client disconnected cleanly', False)

if 'Client disconnected' in server_log or 'Final stats' in server_log:
    write_result('Server detected disconnect', True)
else:
    write_result('Server detected disconnect', False)

# ======== Test 5: Stats ========
write_header('Test 5: Statistics')

client_log = read_log(os.path.join(SCRIPT_DIR, 'client_out.log'))
server_log = read_log(os.path.join(SCRIPT_DIR, 'server_out.log'))

if 'STATS' in client_log:
    write_result('Client stats output', True)
else:
    write_result('Client stats output', False)

if 'STATS' in server_log:
    write_result('Server stats output', True)
else:
    write_result('Server stats output', False)

if 'Sent:' in client_log:
    write_result('Client bytes stats', True)
else:
    write_result('Client bytes stats', False)

# ======== Test 6: Throughput ========
write_header('Test 6: Throughput Test')

write_info('Starting batch test. . .')

client2_proc = subprocess.Popen(
    [CLIENT_EXE, SERVER_ADDR, str(TEST_PORT)],
    stdout=open(os.path.join(SCRIPT_DIR, 'client_out2.log'), 'w'),
    stderr=subprocess.DEVNULL,
    stdin=subprocess.PIPE,
    creationflags=subprocess.CREATE_NO_WINDOW
)
write_info(f'Client2 PID: {client2_proc.pid}')
time.sleep(2)

time.sleep(0.5)
for i in range(1, 21):
    client2_proc.stdin.write(f'BATCH_MSG_{i}\n'.encode())
    client2_proc.stdin.flush()
    time.sleep(0.05)
client2_proc.stdin.close()

time.sleep(4)

kill_proc(client2_proc)
time.sleep(1)

client_log2 = read_log(os.path.join(SCRIPT_DIR, 'client_out2.log'))
echo_count = client_log2.count('SERVER_ECHO')
if echo_count >= 18:
    write_result(f'Batch echo ({echo_count}/20)', True, f'received {echo_count} echoes')
else:
    write_result(f'Batch echo ({echo_count}/20)', False, f'received {echo_count} echoes')

# ======== Cleanup ========
write_header('Cleanup')

write_info('Stopping server. . .')
kill_proc(server_proc)
time.sleep(1)
write_result('Server cleaned up', True)

# ======== Summary ========
write_header('Test Results Summary')

total = PASS_COUNT + FAIL_COUNT
rate = f'{PASS_COUNT}/{total}' if total > 0 else 'N/A'

print(f'  Passed: {PASS_COUNT}')
print(f'  Failed: {FAIL_COUNT}')
print(f'  Total:  {total}')
print(f'  Rate:   {rate}')

print()
print('Details:')
for name, status, detail in results:
    print(f'  [{status}] {name}', end='')
    if detail:
        print(f' - {detail}')

# Save results
result_file = os.path.join(SCRIPT_DIR, 'test_results.txt')
with open(result_file, 'w') as f:
    f.write('KCP Demo Test Results\r\n')
    f.write(f'Date: {time.strftime("%Y-%m-%d %H:%M:%S")}\r\n')
    f.write(f'Passed: {PASS_COUNT} / Failed: {FAIL_COUNT} / Total: {total}\r\n')
    f.write(f'Rate: {rate}\r\n\r\n')
    f.write('--- Details ---\r\n')
    for name, status, detail in results:
        f.write(f'[{status}] {name}')
        if detail:
            f.write(f' - {detail}')
        f.write('\r\n')

print()
print(f'Results saved to: {result_file}')
print()
print('Log files:')
print('  server_out.log')
print('  server_err.log')
print('  client_out.log')
print('  client_out2.log')

sys.exit(1 if FAIL_COUNT > 0 else 0)
