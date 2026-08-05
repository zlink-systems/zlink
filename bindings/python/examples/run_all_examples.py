import os
import signal
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


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
    env = dict(os.environ)
    env["PYTHONPATH"] = (
        str(ROOT / "src")
        + os.pathsep
        + str(ROOT / "samples")
        + os.pathsep
        + str(ROOT / "examples")
    )
    result = _run_python_script(
        ROOT / "samples" / "run_samples.py",
        cwd=ROOT,
        env=env,
        timeout=120,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
