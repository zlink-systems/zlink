import collections, json, pathlib, statistics

job = pathlib.Path(__file__).resolve().parent
runs = []
for line in (job / "summary.jsonl").read_text().splitlines():
    row = json.loads(line)
    stem = pathlib.Path(row["raw"])
    samples = [json.loads(item) for item in stem.with_suffix(".proc.jsonl").read_text().splitlines()]
    depths = []
    for item in samples:
        mem = item.get("memory")
        if not mem or not 0.15 < item["t"] < 4.8:
            continue
        lo, hi = mem.get("backlog_lo"), mem.get("backlog_hi")
        if lo is not None and hi is not None and hi >= lo and -50 < lo < 100000:
            depths.append((lo + hi) / 2)
    runs.append({"run": row["run"], "mean_ms": row["values"].get("latency"),
                 "p95_ms": row["values"].get("latency_p95"),
                 "throughput_per_s": row["values"].get("throughput"),
                 "queue_approx": statistics.mean(depths) if depths else None,
                 "queue_samples": len(depths), "rc": row["rc"], "raw": row["raw"]})
means = [r["mean_ms"] for r in runs]
median = statistics.median(means)
result = {"n": len(runs), "median_mean_ms": median,
          "mean_min_ms": min(means), "mean_max_ms": max(means),
          "max_min_ratio": max(means) / min(means),
          "within_5_percent_of_median": all(abs(x - median) / median <= 0.05 for x in means),
          "runs": runs}
(job / "raw/aa-analysis.json").write_text(json.dumps(result, indent=2) + "\n")
with (job / "raw/aa-runs.tsv").open("w") as f:
    f.write("run\tmean_ms\tp95_ms\tthroughput_per_s\tqueue_approx\tqueue_samples\trc\traw\n")
    for r in runs:
        f.write("\t".join(str(r[k]) for k in ("run", "mean_ms", "p95_ms", "throughput_per_s", "queue_approx", "queue_samples", "rc", "raw")) + "\n")
print(json.dumps({k: v for k, v in result.items() if k != "runs"}, indent=2))
for r in runs:
    print(r["run"], r["mean_ms"], r["p95_ms"], r["throughput_per_s"], r["queue_approx"], r["queue_samples"], r["rc"])
