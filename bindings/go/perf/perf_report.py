#!/usr/bin/env python3
"""Go perf adapter for the shared report renderer."""

import importlib.util
import sys
from pathlib import Path


SHARED_REPORT = Path(__file__).resolve().parents[2] / "python" / "perf" / "perf_report.py"
SPEC = importlib.util.spec_from_file_location("zlink_shared_perf_report", SHARED_REPORT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load shared perf report: {SHARED_REPORT}")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)

# The shared renderer already selects this classifier only for --suite multi.
# Keep the classifier on basename tokens so single and multi can share names.
REPORT.ECHO_MULTI_PATTERNS = {
    "DEALER_ROUTER",
    "DEALER_ROUTER_SENDSEND",
    "DEALER_ROUTER_REQREP",
    "ROUTER_ROUTER",
    "ROUTER_ROUTER_SENDSEND",
    "ROUTER_ROUTER_REQREP",
    "STREAM",
}


def without_msg_unit(lines):
    """Remove only the display-only MsgUnit column from Markdown tables."""
    drop_index = None
    for line in lines:
        if "|" not in line:
            drop_index = None
            yield line
            continue
        fields = line.split("|")
        if "MsgUnit(B)" in line:
            drop_index = next(
                index
                for index, field in enumerate(fields)
                if field.strip() == "MsgUnit(B)"
            )
        if drop_index is None or drop_index >= len(fields):
            yield line
            continue
        yield "|".join(
            field for index, field in enumerate(fields) if index != drop_index
        )


shared_single_auto_hwm_detail_lines = REPORT.single_auto_hwm_detail_lines
shared_multi_auto_hwm_lines = REPORT.multi_auto_hwm_lines


def single_auto_hwm_detail_lines(*args, **kwargs):
    return without_msg_unit(shared_single_auto_hwm_detail_lines(*args, **kwargs))


def multi_auto_hwm_lines(*args, **kwargs):
    return without_msg_unit(shared_multi_auto_hwm_lines(*args, **kwargs))


REPORT.single_auto_hwm_detail_lines = single_auto_hwm_detail_lines
REPORT.multi_auto_hwm_lines = multi_auto_hwm_lines


if __name__ == "__main__":
    REPORT.main(sys.argv[1:])
