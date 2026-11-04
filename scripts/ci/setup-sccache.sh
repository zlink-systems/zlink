#!/usr/bin/env bash
set -uo pipefail

: "${GITHUB_ENV:?GITHUB_ENV must point to the job environment file}"

# Use the runner's server if it already answers; start it only when it does not.
for attempt in 1 2; do
  if sccache --show-stats >/dev/null 2>&1 \
    || { sccache --start-server && sccache --show-stats >/dev/null 2>&1; }; then
    printf 'ZLINK_SCCACHE_LAUNCHER=sccache\n' >> "${GITHUB_ENV}"
    exit 0
  fi
done

echo "::warning::sccache server did not respond after two start/check attempts; building without the compiler cache"
printf 'ZLINK_SCCACHE_LAUNCHER=\n' >> "${GITHUB_ENV}"
