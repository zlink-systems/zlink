#!/usr/bin/env bash
# Usage: run-template.sh <lang> <template> <path> <package>
#
# Runs one genre template the way framework-templates.yml does: copies <path> (a my-game
# directory) under $TEMPLATE_WORK, links the copy to local packages when
# TEMPLATE_SOURCE=local, then runs `./run-e2e.sh --local --build-only` and the Docker e2e
# `./run-e2e.sh`. The template source is never modified.
#
# Environment: TEMPLATE_WORK (work root), TEMPLATE_SOURCE (public|local); for local also
# PACKAGES_DIR and FRAMEWORK_VERSION. Writes $TEMPLATE_WORK/result/<lang>-<template>.json
# and exits non-zero when the build or the e2e fails. Logs stay in the copy's .logs/.
set -euo pipefail
lang=$1 template=$2 path=$3 package=$4
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
game="$TEMPLATE_WORK/$lang-$template/my-game"
result_dir="$TEMPLATE_WORK/result"
rm -rf "$(dirname "$game")"
mkdir -p "$(dirname "$game")" "$result_dir"
cp -R "$repo_root/$path" "$game"
mkdir -p "$game/.logs"

# The Docker build compiles inside the container, where the workflow's
# CMAKE_BUILD_PARALLEL_LEVEL does not reach; without a limit it starts a compiler per core
# on the shared self-hosted build machine.
if [ "$package" = cpp ]; then
  sed -i "s#^FROM \${UBUNTU_IMAGE} AS build\$#&\nENV CMAKE_BUILD_PARALLEL_LEVEL=$CMAKE_BUILD_PARALLEL_LEVEL#" "$game/Dockerfile"
  grep -q "^ENV CMAKE_BUILD_PARALLEL_LEVEL=$CMAKE_BUILD_PARALLEL_LEVEL\$" "$game/Dockerfile" \
    || { echo "run-template: no build stage in $game/Dockerfile" >&2; exit 1; }
fi

if [ "$TEMPLATE_SOURCE" = local ]; then
  binding_version="$(sed -n 's/^ZLINK_BINDING_VERSION=//p' "$repo_root/bindings/$package/VERSION")"
  python3 "$repo_root/scripts/templates/use-local-packages.py" \
    "$lang" "$game" "$PACKAGES_DIR" "$FRAMEWORK_VERSION" "$binding_version"
fi

build=fail e2e=skipped scenarios=""
if (cd "$game" && ./run-e2e.sh --local --build-only) 2>&1 </dev/null | tee "$game/.logs/ci-build.log"; then
  build=pass
  e2e=fail
  e2e_exit=0
  (cd "$game" && ./run-e2e.sh) 2>&1 </dev/null | tee "$game/.logs/ci-e2e.log" || e2e_exit=$?
  # Recorded for failed runs too, so the summary shows how far the scenarios got.
  scenarios="$(grep -o 'Scenarios: [0-9]*/[0-9]* passed' "$game/.logs/ci-e2e.log" | tail -n1 | sed 's/Scenarios: //; s/ passed//' || true)"
  # A run that exits 0 without the scenario line did not run the scenarios.
  [ "$e2e_exit" -eq 0 ] && [ -n "$scenarios" ] && e2e=pass
fi

printf '{"lang":"%s","template":"%s","build":"%s","e2e":"%s","scenarios":"%s"}\n' \
  "$lang" "$template" "$build" "$e2e" "$scenarios" >"$result_dir/$lang-$template.json"
echo "$lang $template: build=$build e2e=$e2e scenarios=${scenarios:-none}"
[ "$build" = pass ] && [ "$e2e" = pass ]
