#!/usr/bin/env python3
"""Connection-memory study runner for zlink core.

Starts a measured server process per scenario, drives swarm clients against
it, and records the server RSS at each connection-count step. Results land in
results/<scenario>.json plus a combined results/summary.md table.

Usage:
    python3 run_study.py                 # all scenarios, default steps
    python3 run_study.py --quick         # smaller steps for a fast smoke run
    python3 run_study.py --scenario router --scenario spot_all
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

STUDY_DIR = Path(__file__).resolve().parent
BIN_DIR = STUDY_DIR / "build"
RESULTS_DIR = STUDY_DIR / "results"

# Stay below the Linux ephemeral range (32768+): large swarm runs leave tens
# of thousands of TIME_WAIT client ports there that would collide with binds.
BASE_PORT = 21000 + (os.getpid() % 200) * 20


class Proc:
    """Line-oriented wrapper around a study binary."""

    def __init__(self, args, name):
        self.name = name
        self.proc = subprocess.Popen(
            [str(a) for a in args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_until(self, prefix, timeout=120.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"{self.name}: EOF while waiting for '{prefix}'")
            line = line.strip()
            if line:
                print(f"    [{self.name}] {line}")
            if line.startswith(prefix):
                return line
        raise TimeoutError(f"{self.name}: timeout waiting for '{prefix}'")

    def quit(self):
        try:
            self.send("quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def parse_rss(line):
    out = {}
    for key in ("vmrss_kb", "vmhwm_kb", "anon_kb", "priv_kb", "fds"):
        m = re.search(rf"{key}=(\d+)", line)
        out[key] = int(m.group(1)) if m else 0
    return out


def read_sockstat():
    """System-wide TCP socket accounting (kernel side)."""
    out = {}
    try:
        text = Path("/proc/net/sockstat").read_text()
        m = re.search(r"TCP: inuse (\d+) orphan (\d+) tw (\d+) alloc (\d+) mem (\d+)", text)
        if m:
            out = {
                "tcp_inuse": int(m.group(1)),
                "tcp_alloc": int(m.group(4)),
                "tcp_mem_pages": int(m.group(5)),
            }
    except OSError:
        pass
    return out


def measure(server, label):
    server.send("trim")
    server.read_until("TRIMMED")
    server.send(f"rss {label}")
    line = server.read_until(f"RSS {label}")
    stats = parse_rss(line)
    stats["sockstat"] = read_sockstat()
    return stats


def wait_server_count(server, minimum, timeout=180.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        server.send("count")
        line = server.read_until("COUNT")
        if int(line.split()[1]) >= minimum:
            return
        time.sleep(0.5)
    raise TimeoutError(f"server never reached count {minimum}")


def wait_spot_status(server, minimum, timeout=300.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        server.send("status")
        line = server.read_until("STATUS")
        m = re.search(r"connected=(\d+)", line)
        if m and int(m.group(1)) >= minimum:
            return
        time.sleep(0.5)
    raise TimeoutError(f"spot hub never reached connected={minimum}")


def run_swarm_scenario(name, server_args, client_binary, client_args_fn, steps,
                       confirm_fn, burst=False, burst_target="client"):
    """Generic incremental scenario: one server, cumulative client swarms."""
    print(f"== scenario {name}: steps {steps}")
    server = Proc(server_args, f"srv:{name}")
    server.read_until("READY", timeout=30.0)
    time.sleep(0.5)

    records = []
    clients = []
    try:
        records.append({"connections": 0, **measure(server, "baseline")})
        total = 0
        for step in steps:
            total += step
            client = Proc(client_args_fn(step), f"cli:{name}:{total}")
            clients.append(client)
            client.read_until("DONE", timeout=600.0)
            confirm_fn(server, total)
            time.sleep(1.5)
            records.append({"connections": total, **measure(server, f"n{total}")})

        if burst and clients:
            print(f"  -- burst phase on {name} (target={burst_target})")
            if burst_target == "server":
                server.send("burst 20 1024")
                server.read_until("BURST_DONE", timeout=600.0)
                time.sleep(4.0)  # let subscriber drain threads empty the pipes
            else:
                for client in clients:
                    client.send("burst 20 1024")
                for client in clients:
                    client.read_until("BURST_DONE", timeout=600.0)
            time.sleep(1.0)
            records.append({"connections": total, "phase": "after_burst",
                            **measure(server, "after_burst")})
            time.sleep(3.0)
            records.append({"connections": total, "phase": "after_burst_idle",
                            **measure(server, "after_burst_idle")})
    finally:
        for client in clients:
            client.quit()
        server.quit()

    return records


def scenario_router(steps):
    port = BASE_PORT
    return run_swarm_scenario(
        "router",
        [BIN_DIR / "srv_router", port],
        "cli_dealer_swarm",
        lambda n: [BIN_DIR / "cli_dealer_swarm", f"tcp://127.0.0.1:{port}", n],
        steps,
        lambda server, total: wait_server_count(server, total),
        burst=True,
    )


def scenario_stream(steps):
    port = BASE_PORT + 1
    return run_swarm_scenario(
        "stream",
        [BIN_DIR / "srv_stream", port],
        "cli_tcp_swarm",
        lambda n: [BIN_DIR / "cli_tcp_swarm", "127.0.0.1", port, n],
        steps,
        lambda server, total: wait_server_count(server, total),
        burst=True,
    )


def scenario_pub(steps):
    port = BASE_PORT + 2
    return run_swarm_scenario(
        "pub",
        [BIN_DIR / "srv_pub", port],
        "cli_sub_swarm",
        lambda n: [BIN_DIR / "cli_sub_swarm", f"tcp://127.0.0.1:{port}", n],
        steps,
        lambda server, total: time.sleep(1.0),  # client DONE already proves delivery
        burst=True,
        burst_target="server",
    )


def scenario_spot(mode, steps, port_shift):
    pub_port = BASE_PORT + port_shift
    router_port = pub_port + 1
    return run_swarm_scenario(
        f"spot_{mode}",
        [BIN_DIR / "srv_spot_hub", pub_port, router_port, mode],
        "cli_spot_peers",
        lambda n: [BIN_DIR / "cli_spot_peers", f"tcp://127.0.0.1:{pub_port}", n, mode],
        steps,
        lambda server, total: wait_spot_status(server, total),
    )


def per_connection_slope(records):
    """kB per connection from the first to the last steady-state record."""
    steady = [r for r in records if "phase" not in r]
    if len(steady) < 3:
        return None
    first, last = steady[1], steady[-1]
    dconn = last["connections"] - first["connections"]
    if dconn <= 0:
        return None
    return (last["vmrss_kb"] - first["vmrss_kb"]) / dconn


def write_summary(all_results):
    lines = ["# Connection-memory study — raw measurement summary", ""]
    lines.append("| scenario | connections | VmRSS (kB) | anon (kB) | fds | kernel tcp_mem (pages) |")
    lines.append("|---|---|---|---|---|---|")
    for name, records in all_results.items():
        for r in records:
            phase = r.get("phase", "")
            label = f"{r['connections']}{' ' + phase if phase else ''}"
            sockstat = r.get("sockstat", {})
            lines.append(
                f"| {name} | {label} | {r['vmrss_kb']} | {r['anon_kb']} | {r['fds']} "
                f"| {sockstat.get('tcp_mem_pages', '-')} |")
    lines.append("")
    lines.append("| scenario | slope (kB / connection) |")
    lines.append("|---|---|")
    for name, records in all_results.items():
        slope = per_connection_slope(records)
        lines.append(f"| {name} | {slope:.1f} |" if slope is not None else f"| {name} | n/a |")
    (RESULTS_DIR / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"summary written: {RESULTS_DIR / 'summary.md'}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--scenario", action="append", default=None,
                        help="router|stream|pub|spot_all|spot_pubsub (repeatable)")
    args = parser.parse_args()

    if args.quick:
        conn_steps = [200, 300]
        spot_steps = [8, 8]
    else:
        conn_steps = [1000, 1000, 2000, 2000, 4000]  # cumulative: 1k, 2k, 4k, 6k, 10k
        spot_steps = [16, 16, 32, 64]                # cumulative: 16, 32, 64, 128

    scenarios = {
        "router": lambda: scenario_router(conn_steps),
        "stream": lambda: scenario_stream(conn_steps),
        "pub": lambda: scenario_pub(conn_steps),
        # Keep spot scenarios >2000 ports apart: the node derives its control
        # bind endpoint as data port + 1000.
        "spot_all": lambda: scenario_spot("all", spot_steps, 10),
        "spot_pubsub": lambda: scenario_spot("pubsub", spot_steps, 2500),
    }
    selected = args.scenario or list(scenarios)

    RESULTS_DIR.mkdir(exist_ok=True)
    all_results = {}
    for name in selected:
        if name not in scenarios:
            print(f"unknown scenario: {name}")
            sys.exit(1)
        records = scenarios[name]()
        all_results[name] = records
        (RESULTS_DIR / f"{name}.json").write_text(json.dumps(records, indent=2) + "\n")
        slope = per_connection_slope(records)
        if slope is not None:
            print(f"== {name}: ~{slope:.1f} kB per connection (steady-state RSS slope)")

    write_summary(all_results)


if __name__ == "__main__":
    main()
