#!/bin/sh

set -eu

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <command> [args...]" >&2
  exit 64
fi

if ! command -v script >/dev/null 2>&1; then
  exec "$@"
fi

quoted_cmd=""
for arg in "$@"; do
  escaped=$(printf "%s" "$arg" | sed "s/'/'\\\\''/g")
  quoted_cmd="${quoted_cmd} '${escaped}'"
done

exec script -qefc "${quoted_cmd# }" /dev/null
