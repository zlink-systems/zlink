#!/usr/bin/env python3

import json
import pathlib
import sys
import time
import urllib.request


def read_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=1) as response:
        return json.load(response)


source_url, target_url, log_dir, timeout_text = sys.argv[1:]
deadline = time.monotonic() + float(timeout_text)
log_path = pathlib.Path(log_dir)
target_identity = read_json(f"{target_url}/identity")
target_rid = target_identity["nodeRid"]
source_evidence = None
target_evidence = None

while time.monotonic() < deadline:
    try:
        source_evidence = read_json(
            f"{source_url}/relocation/readiness-evidence")
        target_evidence = read_json(
            f"{target_url}/relocation/readiness-evidence")
        peers = source_evidence["routeMesh"]["peers"]
        if any(
            peer["nodeRid"] == target_rid
            and peer["state"] in (1, "Ready")
            for peer in peers
        ):
            break
    except Exception:
        pass
    time.sleep(0.1)
else:
    (log_path / "obs-c11-source-readiness.json").write_text(
        json.dumps(source_evidence, ensure_ascii=False, indent=2)
        if source_evidence is not None else "null\n")
    (log_path / "obs-c11-target-readiness.json").write_text(
        json.dumps(target_evidence, ensure_ascii=False, indent=2)
        if target_evidence is not None else "null\n")
    raise SystemExit(
        f"OBS-C11 target {target_rid} did not become a Ready source peer.")

(log_path / "obs-c11-source-readiness.json").write_text(
    json.dumps(source_evidence, ensure_ascii=False, indent=2))
(log_path / "obs-c11-target-readiness.json").write_text(
    json.dumps(target_evidence, ensure_ascii=False, indent=2))
