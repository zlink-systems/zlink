#!/usr/bin/env python3
"""CPU isolation for measurement holds (PM 2026-11-13 17:38).

While a measurement holds measure.lock, the measured process tree gets CPUs 0-5 (source) and
8-13 (target); every other user-space thread and every movable IRQ gets the housekeeping CPUs
6-7,14-15 (physical cores 3 and 7 on this Ryzen 7 5825U: SMT siblings are n,n+1), so no
other work shares a core with a measured thread.

  isolate.py apply <holder-pid>   (idempotent; called every guard poll during a hold)
  isolate.py restore              (puts back every saved affinity, then deletes the state)
"""
import json
import os
import sys

STATE = "/root/jobs/fx/cx/quiet/isolate-state.json"
SOURCE = set(range(0, 6))
TARGET = set(range(8, 14))
MEASURE = SOURCE | TARGET
HOUSE = {6, 7, 14, 15}
HOUSE_LIST = "6-7,14-15"


def ppid_of(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
        return int(s[s.rindex(")") + 2:].split()[1])
    except (OSError, ValueError):
        return None


def load():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"tids": {}, "irqs": {}}


def save(state):
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, STATE)


def measured_mask(cur):
    if cur <= set(range(0, 8)):
        new = cur & SOURCE
        return new or SOURCE
    if cur <= set(range(8, 16)):
        new = cur & TARGET
        return new or TARGET
    new = cur & MEASURE
    return new or MEASURE


def apply(holder):
    state = load()
    pids = [int(p) for p in os.listdir("/proc") if p.isdigit()]
    parent = {p: ppid_of(p) for p in pids}
    tree = {holder}
    changed = True
    while changed:
        changed = False
        for p, pp in parent.items():
            if pp in tree and p not in tree:
                tree.add(p)
                changed = True
    for p in pids:
        if p in (1, 2) or parent.get(p) == 2:
            continue  # init and kernel threads
        try:
            tids = [int(t) for t in os.listdir(f"/proc/{p}/task")]
        except OSError:
            continue
        for t in tids:
            try:
                cur = os.sched_getaffinity(t)
            except OSError:
                continue
            if p in tree:
                want = measured_mask(cur)
            else:
                want = HOUSE
                if str(t) not in state["tids"]:
                    # a thread or process born during the hold inherited HOUSE: restore it to
                    # its group leader's / parent's original mask, else to all CPUs
                    orig = cur
                    if cur == HOUSE:
                        orig = (state["tids"].get(str(p))
                                or state["tids"].get(str(parent.get(p)))
                                or list(range(os.cpu_count())))
                    state["tids"][str(t)] = sorted(orig)
            if cur != want:
                try:
                    os.sched_setaffinity(t, want)
                except OSError:
                    pass
    for irq in os.listdir("/proc/irq"):
        path = f"/proc/irq/{irq}/smp_affinity_list"
        if not irq.isdigit() or not os.path.exists(path):
            continue
        try:
            with open(path) as f:
                cur = f.read().strip()
            if cur == HOUSE_LIST:
                continue
            state["irqs"].setdefault(irq, cur)
            with open(path, "w") as f:
                f.write(HOUSE_LIST)
        except OSError:
            pass
    save(state)


def restore():
    state = load()
    for t, cpus in state["tids"].items():
        try:
            os.sched_setaffinity(int(t), set(cpus))
        except OSError:
            pass
    for irq, cur in state["irqs"].items():
        try:
            with open(f"/proc/irq/{irq}/smp_affinity_list", "w") as f:
                f.write(cur)
        except OSError:
            pass
    try:
        os.remove(STATE)
    except OSError:
        pass
    print(f"restored tids={len(state['tids'])} irqs={len(state['irqs'])}")


if __name__ == "__main__":
    if sys.argv[1] == "apply":
        apply(int(sys.argv[2]))
    else:
        restore()
