#!/usr/bin/env python3
"""Throughput sanity probe for memory experiments.

Single-dealer deep burst through a ROUTER: blocking sends push the pipe to its
HWM, so the send-side elapsed time approximates sustained pipe throughput
(chunk allocation churn included). Used to compare baseline vs experimental
libzlink builds (e.g. different message_pipe_granularity).

Usage: run_perf_probe.py [--msgs 2000000] [--bytes 64] [--conns 1] [--label x]
"""

import argparse
import os
import re
import time

from run_study import Proc, BIN_DIR, wait_server_count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--msgs", type=int, default=2000000)
    parser.add_argument("--bytes", type=int, default=64)
    parser.add_argument("--conns", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--label", default="baseline")
    args = parser.parse_args()

    port = 36100 + (os.getpid() % 900)
    server = Proc([BIN_DIR / "srv_router", port], "srv:perf")
    server.read_until("READY", timeout=30.0)

    client = Proc([BIN_DIR / "cli_dealer_swarm", f"tcp://127.0.0.1:{port}", args.conns],
                  "cli:perf")
    client.read_until("DONE", timeout=60.0)
    wait_server_count(server, args.conns)

    per_conn = args.msgs // args.conns
    total = args.conns  # initial hello messages already counted
    for round_no in range(args.rounds):
        start = time.monotonic()
        client.send(f"burst {per_conn} {args.bytes}")
        line = client.read_until("BURST_DONE", timeout=600.0)
        send_ms = float(re.search(r"elapsed_ms=([\d.]+)", line).group(1))
        total += per_conn * args.conns
        wait_server_count(server, total, timeout=600.0)
        drain_ms = (time.monotonic() - start) * 1000.0
        rate = per_conn * args.conns / send_ms * 1000.0
        print(f"PERF {args.label} round={round_no} msgs={per_conn * args.conns} "
              f"bytes={args.bytes} send_ms={send_ms:.0f} drain_ms={drain_ms:.0f} "
              f"send_rate_msg_s={rate:.0f}")

    client.quit()
    server.quit()


if __name__ == "__main__":
    main()
