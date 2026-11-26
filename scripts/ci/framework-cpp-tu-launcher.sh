#!/usr/bin/env bash
set -euo pipefail

log=$1
shift

# CMake's Linux Ninja rules pass the source immediately after -c.
args=("$@")
for ((i = 0; i + 1 < ${#args[@]}; ++i)); do
  if [[ "${args[i]}" == -c ]]; then
    source=${args[i+1]}
    if [[ "$source" != /* ]]; then
      source="$PWD/$source"
    fi
    # Escape GNU time's format directives in the source path.
    source=${source//\\/\\\\}
    source=${source//%/%%}
    exec /usr/bin/time -q -a -o "$log" -f "%M"$'\t'"${source}" -- "$@"
  fi
done

exec "$@"
