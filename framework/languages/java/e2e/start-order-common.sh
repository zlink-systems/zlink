#!/usr/bin/env bash

zlink_e2e_start_order_mode() {
  local mode="forward"
  while [[ "$#" -gt 0 ]]; do
    if [[ "$1" == "--start-order" ]]; then
      if [[ "$#" -lt 2 ]]; then
        echo "--start-order requires a value" >&2
        return 2
      fi
      mode="$2"
      shift 2
      continue
    fi
    shift
  done
  printf '%s\n' "${mode}"
}

zlink_e2e_order_roles() {
  local mode
  mode="${e2e_start_order:-forward}"

  python3 - "${mode}" "$@" <<'PY'
import random
import sys

mode = sys.argv[1]
roles = sys.argv[2:]

if mode == "forward":
    pass
elif mode == "reverse":
    roles.reverse()
elif mode.startswith("shuffle:"):
    seed_text = mode.split(":", 1)[1]
    if not seed_text:
        raise SystemExit("--start-order shuffle requires a seed")
    try:
        seed = int(seed_text)
    except ValueError as error:
        raise SystemExit("--start-order shuffle seed must be an integer") from error
    random.Random(seed).shuffle(roles)
else:
    raise SystemExit(f"unsupported --start-order value: {mode!r}")

print("\n".join(roles))
PY
}
