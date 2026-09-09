#!/usr/bin/env bash
# Runs `dotnet test` for one project/configuration/framework and, when tests
# fail, reruns only the failed tests once. Shared CI runners make the
# framework's real-timer tests (5 s probes, 15 s expiries, deadlines) miss by
# scheduler noise; a test that fails twice in a row is reported as a failure.
# Tolerances inside the tests are never changed here.
set -euo pipefail

project="$1"; configuration="$2"; framework="$3"
results="${RUNNER_TEMP:-/tmp}/zlink-test-results/$(basename "${project%.csproj}")-${configuration}-${framework}"
rm -rf "$results"; mkdir -p "$results"

run_tests() {
  local trx="$1"; shift
  dotnet test "$project" -c "$configuration" -f "$framework" --no-build \
    --logger "trx;LogFileName=${trx}" --results-directory "$results" "$@"
}

if run_tests first.trx; then
  exit 0
fi

failed=$(python3 - "$results/first.trx" <<'PY'
import sys, xml.etree.ElementTree as ET
ns = {'t': 'http://microsoft.com/schemas/VisualStudio/TeamTest/2010'}
root = ET.parse(sys.argv[1]).getroot()
failed_ids = {r.get('testId') for r in root.iter('{%s}UnitTestResult' % ns['t']) if r.get('outcome') == 'Failed'}
names = set()
for ut in root.iter('{%s}UnitTest' % ns['t']):
    if ut.get('id') in failed_ids:
        m = ut.find('t:TestMethod', ns)
        name = m.get('name').split('(')[0]
        names.add(f"{m.get('className')}.{name}")
print('|'.join(f'FullyQualifiedName={n}' for n in sorted(names)))
PY
)
if [ -z "$failed" ]; then
  echo "::error::dotnet test failed without any failed test result (build or host error)" >&2
  exit 1
fi
echo "::warning::retrying failed tests once: ${failed//FullyQualifiedName=/}"
run_tests retry.trx --filter "$failed"
