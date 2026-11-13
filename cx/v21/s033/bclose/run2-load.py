import json, os, signal, subprocess, time
from pathlib import Path
root = Path(__file__).parent
(root / "load-run2.pid").write_text(str(os.getpid()) + "\n")
cmd = ["stress-ng", "--cpu", "16", "--cpu-method", "loop", "--metrics-brief", "--log-file", str(root / "load-run2.log")]
load = subprocess.Popen(cmd)
def stop(signum, frame):
    if load.poll() is None:
        load.send_signal(signal.SIGTERM)
signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
lowest = None
with (root / "memory-run2.log").open("w", buffering=1) as memory:
    while True:
        available = int(next(line.split()[1] for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemAvailable:")))
        lowest = available if lowest is None else min(lowest, available)
        memory.write(f"{time.time():.3f} MemAvailable_kB={available}\n")
        if available < 12 * 1024 * 1024:
            memory.write("MEMORY LIMIT VIOLATED\n")
            load.send_signal(signal.SIGTERM)
        try:
            rc = load.wait(timeout=5)
            break
        except subprocess.TimeoutExpired:
            pass
print(json.dumps({"load_exit": rc, "minimum_MemAvailable_kB": lowest, "minimum_GiB": lowest / 1024**2}), flush=True)
