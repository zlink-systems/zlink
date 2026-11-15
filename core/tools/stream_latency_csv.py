#!/usr/bin/env python3
"""Join STREAM diagnostic stage rows; reject reversed timestamps and data loss."""
import argparse
import csv
import glob
import os
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="OUT.PID.TID.csv paths or globs")
    parser.add_argument("--strict", action="store_true", help="also reject incomplete messages")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    paths = sorted({p for pattern in args.inputs for p in glob.glob(pattern)})
    if not paths:
        parser.error("no input files")
    records = {}
    errors = []
    for path in paths:
        # A connection id is process-local; never join distinct producer PIDs.
        pid = os.path.basename(path).rsplit(".", 3)[1]
        with open(path, newline="") as source:
            for row in csv.reader(source):
                if len(row) != 7:
                    errors.append(f"{path}: malformed row: {row}")
                    continue
                conn, seq, direction, stage, ns, tid, value = row
                if stage == "loss":
                    errors.append(f"{path}: lost diagnostic records: {value}")
                if direction == "io":
                    continue
                key = (pid, int(conn), int(seq), direction)
                record = records.setdefault(key, {})
                if stage in record:
                    errors.append(f"{key}: duplicate stage {stage}")
                record[stage] = int(ns)
                if stage in ("read", "write"):
                    record["io_thread_id"] = tid
    complete = incomplete = 0
    fields = ["pid", "conn_id", "seq", "direction", "read_ns", "enqueue_ns",
              "deliver_ns", "accept_ns", "write_ns", "io_thread_id"]
    with open(args.out, "w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        for (pid, conn, seq, direction), record in sorted(records.items()):
            stages = ("read", "rx_enqueue", "deliver") if direction == "rx" else (
                "accept", "tx_enqueue", "write")
            present = [record[s] for s in stages if s in record]
            if any(a > b for a, b in zip(present, present[1:])):
                errors.append(f"{(pid, conn, seq, direction)}: reversed timestamps")
            if all(s in record for s in stages):
                complete += 1
            else:
                incomplete += 1
                if args.strict:
                    errors.append(f"{(pid, conn, seq, direction)}: incomplete {record}")
            writer.writerow(dict(pid=pid, conn_id=conn, seq=seq, direction=direction,
                read_ns=record.get("read", ""),
                enqueue_ns=record.get("rx_enqueue" if direction == "rx" else "tx_enqueue", ""),
                deliver_ns=record.get("deliver", ""), accept_ns=record.get("accept", ""),
                write_ns=record.get("write", ""), io_thread_id=record.get("io_thread_id", "")))
    print(f"files={len(paths)} complete={complete} incomplete={incomplete} errors={len(errors)}")
    for error in errors[:20]:
        print(error, file=sys.stderr)
    return 1 if errors or not records else 0


if __name__ == "__main__":
    sys.exit(main())
