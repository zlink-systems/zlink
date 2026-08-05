import os
import signal
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SAMPLES_DIR = ROOT / "samples"
CANONICAL_SAMPLES = [
    "request_reply_callback_sample.py",
    "pair_recv_sample.py",
    "dealer_router_recv_sample.py",
    "pubsub_recv_sample.py",
    "stream_recv_sample.py",
    "stream_packet_callback_sample.py",
    "monitor_recv_sample.py",
]


def _verify_canonical_samples():
    missing = [name for name in CANONICAL_SAMPLES if not (SAMPLES_DIR / name).exists()]
    if missing:
        raise SystemExit(
            "canonical sample file missing: " + ", ".join(sorted(missing))
        )


def _run_python_script(script_path, *, timeout, cwd, env):
    proc = subprocess.Popen(
        [sys.executable, str(script_path)],
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            stdout, stderr = proc.communicate()
    return subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)


def main():
    arguments = sys.argv[1:]
    if arguments not in ([], ["--installed"]):
        raise SystemExit("usage: run_samples.py [--installed]")
    installed = arguments == ["--installed"]
    _verify_canonical_samples()
    env = dict(os.environ)
    if installed:
        # The clean-consumer mode must resolve zlink from the installed wheel,
        # while still making sample_support importable from the sample tree.
        env["PYTHONPATH"] = str(SAMPLES_DIR)
        run_cwd = Path.cwd()
    else:
        env["PYTHONPATH"] = os.pathsep.join(
            [
                str(ROOT / "src"),
                str(SAMPLES_DIR),
                env.get("PYTHONPATH", ""),
            ]
        ).rstrip(os.pathsep)
        run_cwd = ROOT

    passed = 0
    for name in CANONICAL_SAMPLES:
        sample_path = SAMPLES_DIR / name
        result = _run_python_script(
            sample_path,
            cwd=run_cwd,
            env=env,
            timeout=120,
        )
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            raise SystemExit(f"sample failed: {name}")
        passed += 1

    print(f"Summary: {passed}/{len(CANONICAL_SAMPLES)} samples passed")


if __name__ == "__main__":
    main()
