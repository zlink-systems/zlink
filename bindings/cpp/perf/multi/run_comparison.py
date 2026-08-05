#!/usr/bin/env python3
import importlib.util
import os
import sys
from pathlib import Path


def load_runner_module():
    os.environ["PERF_ALLOW_MULTI"] = "1"
    runner = Path(__file__).resolve().parents[1] / "run_comparison.py"
    spec = importlib.util.spec_from_file_location("cpp_perf_run_comparison", runner)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load runner: {runner}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    runner = load_runner_module()
    return runner.main()


if __name__ == "__main__":
    raise SystemExit(main())
