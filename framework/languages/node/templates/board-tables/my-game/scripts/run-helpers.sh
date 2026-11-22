# Shared by run-server.sh and run-e2e.sh.

# npm ci replaces node_modules, which a running server still uses, so a build must not start while
# the server recorded in logs/server.json still answers on its control port.
assert_no_running_server() {
  local state="$1/logs/server.json" endpoint address
  [ -f "$state" ] || return 0
  endpoint="$(sed -n 's/.*"controlEndpoint":"http:\/\/\([^"]*\)".*/\1/p' "$state")"
  [ -n "$endpoint" ] || return 0
  address="${endpoint%%/*}"
  if (exec 3<>"/dev/tcp/${address%:*}/${address##*:}") 2>/dev/null; then
    echo 'A server is already running (logs/server.json). Stop it with Ctrl+C in its terminal first.' >&2
    exit 1
  fi
}
