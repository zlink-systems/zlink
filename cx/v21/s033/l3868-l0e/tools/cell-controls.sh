#!/usr/bin/env bash
record_cell_controls() {
  python3 - "$1" "$2" "${TARGET_RATE:-0}" "${SOURCE_CPUS:-}" "${TARGET_CPUS:-}" <<'PY'
import json, os, sys, urllib.request
path, source_url, rate, source_cpus, target_cpus = sys.argv[1:]
with open(path, encoding="utf-8") as handle:
    result = json.load(handle)
with urllib.request.urlopen(source_url + "/bench/stats", timeout=5) as response:
    source = json.load(response)
metadata = result.setdefault("metadata", {})
metadata["submitConcurrency"] = source["submitConcurrency"]
cell = result["cells"][0]
cell.setdefault("streams", source["streams"])
if cell.get("contaminated"):
    cell["submitted"] = source["submitted"]
    cell["completed"] = source["completed"]
    cell["abandoned"] = source["currentInFlight"]
    cell["errors"] = source["errors"]
    cell["client_error_summary"] = source["client_error_summary"]
    cell["client_error_other_count"] = source["client_error_other_count"]
value = float(rate)
metadata["targetRate"] = int(value) if value.is_integer() else value
metadata["sourceCpus"] = source_cpus or "unpinned"
metadata["targetCpus"] = target_cpus or "unpinned"
temporary = path + ".controls"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
    handle.write("\n")
os.replace(temporary, path)
PY
}

