#!/usr/bin/env bash
# Tests download() of scripts/local-package/core/fetch-release.sh: gh first when
# GH_TOKEN is set, public curl with retries otherwise (#4065). No network.

set -euo pipefail

TEST_ROOT=/tmp/zlink-fetch-release-download-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
SCRIPT="$SOURCE_ROOT/scripts/local-package/core/fetch-release.sh"
PASS=0
SERVER_PID=""

cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" >/dev/null 2>&1
    [[ "$TEST_ROOT" == /tmp/zlink-fetch-release-download-test.* ]] && rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

fail() {
    printf "FAIL: %s\n" "$*" >&2
    exit 1
}

pass() {
    PASS=$((PASS + 1))
    printf "ok %d - %s\n" "$PASS" "$1"
}

mkdir -p "$TEST_ROOT/bin" "$TEST_ROOT/out"

# Load only the retry setup and download() from the script under test.
sed -n "/^curl_retry_args=/,/^}/p" "$SCRIPT" >"$TEST_ROOT/download.sh"
grep -q "^download() {" "$TEST_ROOT/download.sh" || fail "download() not found in fetch-release.sh"
# shellcheck disable=SC2034 # read by download()
release_tag="core/v9.9.9"
export ZLINK_CORE_DOWNLOAD_RETRY_DELAY=1
# shellcheck disable=SC1091
source "$TEST_ROOT/download.sh"

# Local HTTP server: /flaky.txt answers 504 twice, then 200.
cat >"$TEST_ROOT/server.py" <<PY
import http.server, sys
hits = {}
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        hits[self.path] = hits.get(self.path, 0) + 1
        if hits[self.path] <= 2:
            self.send_response(504); self.end_headers(); return
        body = b"public-ok\n"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
PORT=$(python3 -c "import socket;s=socket.socket();s.bind((\"127.0.0.1\",0));print(s.getsockname()[1]);s.close()")
python3 "$TEST_ROOT/server.py" "$PORT" &
SERVER_PID=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/ping" 2>/dev/null && break
    sleep 0.1
done
# The readiness probe counted one hit on /ping only; use fresh paths below.

unset GH_TOKEN || true
SAVED_PATH=$PATH

# (a) public path: 504 twice then 200
out="$TEST_ROOT/out/a.txt"
download "http://127.0.0.1:$PORT/a/flaky.txt" "$out" 2>"$TEST_ROOT/a.err" || { cat "$TEST_ROOT/a.err" >&2; fail "(a) download failed"; }
grep -q "^public-ok$" "$out" || fail "(a) wrong content"
pass "public curl retries through two 504 responses"

# Fake gh recording arguments.
cat >"$TEST_ROOT/bin/gh" <<SH
#!/usr/bin/env bash
printf "%s\n" "\$*" >>"$TEST_ROOT/gh.args"
printf "%s\n" "\$GH_TOKEN" >"$TEST_ROOT/gh.token.seen"
if [[ -f "$TEST_ROOT/gh.fail" ]]; then exit 1; fi
out=""
while [[ \$# -gt 0 ]]; do [[ "\$1" == -O ]] && out=\$2; shift; done
printf "gh-ok\n" >"\$out"
SH
chmod +x "$TEST_ROOT/bin/gh"

# (b) token path uses gh with the right tag and asset
export GH_TOKEN=test-token
PATH="$TEST_ROOT/bin:$SAVED_PATH"
out="$TEST_ROOT/out/b.txt"
download "http://127.0.0.1:$PORT/releases/download/core/v9.9.9/checksums.txt" "$out" 2>"$TEST_ROOT/b.err" || fail "(b) download failed"
grep -q "^gh-ok$" "$out" || fail "(b) gh output not used"
expected="release download core/v9.9.9 -R zlink-systems/zlink -p checksums.txt -O $out --clobber"
[[ "$(cat "$TEST_ROOT/gh.args")" == "$expected" ]] || fail "(b) gh args: $(cat "$TEST_ROOT/gh.args")"
grep -q "test-token" "$TEST_ROOT/b.err" && fail "(b) token printed"
pass "token path calls gh release download with tag and asset name"

# (c) gh fails -> falls back to curl (public URL, 504 twice then 200)
: >"$TEST_ROOT/gh.args"
touch "$TEST_ROOT/gh.fail"
out="$TEST_ROOT/out/c.txt"
download "http://127.0.0.1:$PORT/c/flaky.txt" "$out" 2>"$TEST_ROOT/c.err" || { cat "$TEST_ROOT/c.err" >&2; fail "(c) fallback failed"; }
grep -q "^public-ok$" "$out" || fail "(c) wrong fallback content"
[[ -s "$TEST_ROOT/gh.args" ]] || fail "(c) gh was not tried first"
grep -q "falling back" "$TEST_ROOT/c.err" || fail "(c) no fallback message"
pass "gh failure falls back to the public URL"

printf "%d tests passed\n" "$PASS"
