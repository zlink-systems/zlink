#!/usr/bin/env python3
"""Compatibility entrypoint; the shared contract owns this implementation."""
from pathlib import Path
import runpy
import sys
shared=Path(__file__).resolve().parents[5]/"framework/perf-contract"
sys.path.insert(0,str(shared))
globals().update(runpy.run_path(str(shared/"results.py"),run_name="__main__" if __name__=="__main__" else "shared_results"))
