server_status() {
  [[ -f .logs/server.json && -f dist/e2e/runner.js ]] &&
    node dist/e2e/runner.js "$1"
}

assert_no_running_server() {
  if server_status --server-active; then
    echo 'A server is already starting or running. Stop it with Ctrl+C before building.' >&2
    exit 1
  fi
}

build_local() {
  echo 'Preparing: building the server (Node.js)...'
  if ! (npm ci && npm run build) >.logs/build.log 2>&1; then
    tail -n 20 .logs/build.log >&2
    return 1
  fi
  echo 'Build complete. Starting servers...'
}
