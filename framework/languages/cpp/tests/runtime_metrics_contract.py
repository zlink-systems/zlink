#!/usr/bin/env python3
"""Check the runtime instrument surface against the shared metric contract."""
import json
import re
import sys
from pathlib import Path
root = Path(__file__).resolve().parents[3]
fixture = json.loads((root / "runtime/protocol/fixtures/runtime-metrics-v1.json").read_text(encoding="utf-8"))
contract = {item["name"]: item for item in fixture["metrics"]}
expected = set(contract)
sources = root / "languages/cpp/framework/src/runtime"
actual = set()
metadata_checked = set()
metadata_errors = []
instrument_kinds = {
    "Counter": "counter",
    "ObservableCounter": "counter",
    "UpDownCounter": "updown",
    "ObservableGauge": "observable",
    "Histogram": "histogram",
}
for path in sources.rglob("*"):
    if path.suffix in (".cpp", ".hpp"):
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
        actual.update(re.findall(r'"(zlink\.[a-z_]+(?:\.[a-z_]+)+)"', source))
        declarations = [
            (name, instrument_kinds[kind], unit)
            for kind, name, unit in re.findall(
                r'Create(?:Double|Int64|UInt64)(ObservableCounter|UpDownCounter|Counter|'
                r'ObservableGauge|Histogram)\s*\(\s*"(zlink\.[a-z_.]+)"\s*,\s*"[^"]*"\s*,\s*"([^"]*)"',
                source,
            )
        ]
        declarations += [
            (name, kind, unit)
            for kind, name, unit in re.findall(
                r'\.(counter|updown|observable|histogram)\s*\(\s*"(zlink\.[a-z_.]+)"\s*,\s*"([^"]*)"',
                source,
            )
        ]
        declarations += re.findall(
            r'"(zlink\.[a-z_.]+)"\s*,\s*(?:::[\w:]+::)?metric_instrument_kind_t::'
            r'(counter|updown|observable|histogram)\s*,\s*"([^"]*)"',
            source,
        )
        for name, kind, unit in declarations:
            if name not in contract:
                continue
            metadata_checked.add(name)
            if (kind, unit) != (contract[name]["kind"], contract[name]["unit"]):
                metadata_errors.append(f"{path.name}: {name}: {kind}, {unit}")
actual = {name for name in actual if not name.startswith(("zlink.runtime.", "zlink.framework."))}
missing = sorted(expected - actual)
extra = sorted(actual - expected)


def spot_recording_labels(language, source, catalog=""):
    """Read the labels passed by recording paths, rather than catalog labels."""
    if language == "cpp":
        calls = re.findall(
            r'\.updown\s*\(\s*"zlink\.spot\.count"\s*,\s*"\{spot\}"\s*,\s*-?1\s*,(.*?)\);',
            source, re.S,
        )
        labels = [set(re.findall(r'\{\s*"([a-z_]+)"\s*,', call)) for call in calls]
    elif language == "dotnet":
        constants = dict(re.findall(r'const string (\w+) = "([a-z_]+)";', source))
        calls = re.findall(r'SafeAdd\(SpotCount,\s*-?1,\s*new TagList\s*\{(.*?)\}\);', source, re.S)
        labels = [set(constants[key] for key in re.findall(r'\{\s*(\w+)\s*,', call)) for call in calls]
    elif language == "java":
        constants = dict(re.findall(r'\b([A-Z_]+)\("([a-z_]+)"\)', catalog))
        calls = re.findall(r'void recordSpotCount\([^)]*\)\s*\{(.*?)\.add\(delta\);', source, re.S)
        labels = [set(constants[key] for key in re.findall(r'Tag\.(\w+)\.wire\(\)', call)) for call in calls]
        if not all("Metric.SPOT_COUNT" in call for call in calls):
            raise ValueError("Java Spot population no longer uses the shared catalog")
    elif language == "node":
        calls = re.findall(r'this\.change\(METRIC_NAMES\.SpotCount,\s*delta,\s*\{([^}]+)\}\)', source)
        labels = [set(re.findall(r'([a-z_]+)\s*:', call)) for call in calls]
    else:
        raise ValueError(language)
    if not labels:
        raise ValueError(f"{language}: no actual Spot count recording path found")
    return labels


spot_sources = {
    "cpp": root / "languages/cpp/framework/src/runtime/spots/spot_runtime.cpp",
    "dotnet": root / "languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkRuntimeMetrics.cs",
    "java": root / "languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotLifecycle.java",
    "node": root / "languages/node/packages/framework/src/runtime/diagnostics/runtime-metrics.ts",
}
java_catalog = (root / "languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/metrics/ZLinkRuntimeMetrics.java").read_text(encoding="utf-8")
spot_expected = set(contract["zlink.spot.count"]["labels"])
for language, path in spot_sources.items():
    source = path.read_text(encoding="utf-8")
    try:
        series = spot_recording_labels(language, source, java_catalog)
        for labels in series:
            if labels != spot_expected:
                metadata_errors.append(f"{path.relative_to(root)}: zlink.spot.count labels {sorted(labels)} != {sorted(spot_expected)}")
        print(f"{language} actual Spot recording labels ({len(series)} paths): {sorted(set.union(*series))}")
        # A wrong label must fail even if the instrument catalog stays correct.
        if language == "java":
            invalid = spot_recording_labels(language, source, java_catalog.replace('MESH_NAME("mesh_name")', 'MESH_NAME("kind")'))
        elif language == "node":
            invalid = spot_recording_labels(language, source.replace('mesh_name: meshName, spot_kind: kind', 'kind: kind'), java_catalog)
        else:
            invalid = spot_recording_labels(language, source.replace('"mesh_name"', '"kind"'), java_catalog)
        if not any(labels != spot_expected for labels in invalid):
            metadata_errors.append(f"{language}: negative label check did not reject a malformed recording")
    except (KeyError, ValueError) as failure:
        metadata_errors.append(str(failure))

def spot_recording_values(language, texts):
    """Follow activation kind into both population increments and decrements."""
    created, closed = set(), set()
    if language == "cpp":
        source = texts["runtime"]
        emissions = re.findall(r'\.updown\s*\("zlink\.spot\.count"(.*?)\);', source, re.S)
        if not emissions or not all(re.search(r'\{"spot_kind",\s*kind\}', call) for call in emissions):
            raise ValueError("cpp: Spot recording does not preserve the selected kind")
        create = re.search(r'const auto kind = ([^;]+);\s*metrics\.updown\s*\("zlink\.spot\.count"', source)
        close = re.search(r'const char \*metric_spot_kind\s*\([^)]*\)[^{]*\{([^}]+)\}', source)
        if create and close and re.search(r'result\.kind = metric_spot_kind\s*\(', source):
            created = set(re.findall(r'"([a-z]+)"', create[1]))
            closed = set(re.findall(r'"([a-z]+)"', close[1]))
    elif language == "dotnet":
        emissions = re.findall(r'SafeAdd\(SpotCount,(.*?)\);', texts["metrics"], re.S)
        if not emissions or not all(re.search(r'\{\s*SpotKindTag,\s*kind\s*\}', call) for call in emissions):
            raise ValueError("dotnet: Spot recording does not preserve the activation kind")
        constants = dict(re.findall(r'const string (\w+) = "([a-z_]+)";', texts["metrics"]))
        callers = texts["catalog"] + texts["node"]
        created = set(re.findall(r'RecordSpotCreated\([^;]*?,\s*"([a-z]+)"\s*\)', callers))
        closed = set(re.findall(r'RecordSpotClosed\([^;]*?,\s*"([a-z]+)"\s*\)', callers))
        if re.search(r'RecordSpotClosed\([^;]*activation\.KindName\s*\)', callers):
            closed.update(constants[key] for key in re.findall(r'KindName => ZLinkRuntimeMetrics\.(\w+)', texts["activation"]))
    elif language == "java":
        if not re.search(r'Tag\.SPOT_KIND\.wire\(\),\s*kind\.wire\(\)', texts["lifecycle"]):
            raise ValueError("java: Spot recording does not preserve the activation kind")
        callers = texts["lifecycle"] + texts["runtime"]
        for kind, delta in re.findall(r'recordSpotCount\([^;]*?SpotKind\.(\w+),\s*(-?1|delta)\s*\)', callers):
            if delta in ("1", "delta"):
                created.add(kind.lower())
            if delta in ("-1", "delta"):
                closed.add(kind.lower())
        # The typed Instance activation must reach the shared recorder in both directions.
        for delta in ("1", "-1"):
            if not re.search(r'recordInstanceCount\(activation,\s*' + re.escape(delta) + r'\)', texts["runtime"]):
                raise ValueError(f"java: missing Instance population delta {delta}")
    elif language == "node":
        if not re.search(r'SpotCount,\s*delta,\s*\{\s*mesh_name:\s*meshName,\s*spot_kind:\s*kind\s*\}', texts["metrics"]):
            raise ValueError("node: Spot recording does not preserve the activation kind")
        registry = texts["registry"]
        if re.search(r'recordSpotCount\(activation\.meshName,\s*activation\.domain\.kind,\s*delta\)', registry):
            domains = re.findall(r'domain:\s*(.*?)(?=\n\s*spotType:)', texts["activation"], re.S)
            kinds = set(re.findall(r"kind:\s*'([a-z]+)'", "\n".join(domains)))
            if re.search(r'recordPopulation\(activation,\s*1\)', registry):
                created.update(kinds)
            if re.search(r'recordPopulation\(activation,\s*-1\)', registry):
                closed.update(kinds)
        for kind, delta in re.findall(r"recordSpotCount\([^;]*?,\s*'([a-z]+)',\s*(-?1)\)", texts["entry"]):
            (created if delta == "1" else closed).add(kind)
    return {"created": created, "closed": closed}


value_paths = {
    "cpp": {"runtime": spot_sources["cpp"]},
    "dotnet": {
        "metrics": spot_sources["dotnet"],
        "catalog": root / "languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeCatalog.cs",
        "activation": root / "languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotActivation.cs",
        "node": root / "languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeRuntime.cs",
    },
    "java": {
        "lifecycle": spot_sources["java"],
        "runtime": root / "languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRuntime.java",
    },
    "node": {
        "metrics": spot_sources["node"],
        "registry": root / "languages/node/packages/framework/src/runtime/spots/spot-activation-registry.ts",
        "activation": root / "languages/node/packages/framework/src/runtime/spots/spot-activation.ts",
        "entry": root / "languages/node/packages/framework/src/runtime/spots/spot-entry-activation.ts",
    },
}
allowed_values = set(contract["zlink.spot.count"]["labelValues"]["spot_kind"])
for language, paths in value_paths.items():
    texts = {key: path.read_text(encoding="utf-8") for key, path in paths.items()}
    try:
        values = spot_recording_values(language, texts)
        for direction, kinds in values.items():
            if kinds != allowed_values:
                metadata_errors.append(f"{language} Spot {direction} values: {sorted(kinds)} != {sorted(allowed_values)}")
        invalid = {key: source.replace('"instance"', '"user"').replace("'instance'", "'user'").replace('SpotKind.INSTANCE', 'SpotKind.USER') for key, source in texts.items()}
        if all(kinds == allowed_values for kinds in spot_recording_values(language, invalid).values()):
            metadata_errors.append(f"{language}: instance-to-user negative check was not rejected")
        print(f"{language} actual Spot values: created={sorted(values['created'])}, closed={sorted(values['closed'])}; instance-to-user mutation rejected")
    except (KeyError, ValueError) as failure:
        metadata_errors.append(str(failure))

if missing or extra or metadata_errors:
    print("Missing:", *missing, sep="\n  ")
    print("Outside contract:", *extra, sep="\n  ")
    print("Incorrect kind/unit:", *metadata_errors, sep="\n  ")
    sys.exit(1)
print(f"Metric names match: {len(expected)}; literal kind/unit declarations: {len(metadata_checked)}")

