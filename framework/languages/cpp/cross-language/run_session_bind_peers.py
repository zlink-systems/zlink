"""Check Node Session owners against the Java and C++ Actor owner fixtures."""

import json
from pathlib import Path
import selectors
import subprocess
import time

ROOT = Path(__file__).resolve().parents[4]


class Host:
    def __init__(self, args, log):
        self.stderr = log.open("w")
        self.process = subprocess.Popen(
            args, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.stderr, text=True, bufsize=1,
        )

    def read(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(10):
                raise TimeoutError("Session bind host did not reply")
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"Session bind host exited: {self.process.poll()}")
        value = json.loads(line)
        print(json.dumps(value), flush=True)
        return value

    def command(self, **value):
        self.process.stdin.write(json.dumps(value) + "\n")
        self.process.stdin.flush()
        return self.read()

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.stderr.close()


def verify(language):
    log = ROOT / "logs" / f"node-bind-{language}"
    log.mkdir(parents=True, exist_ok=True)
    peer = Host([
        "bash", str(ROOT / "framework/languages/cpp/cross-language/launch_session_bind_peer.sh"),
        language, "tcp://127.0.0.1:0", "mesh",
    ], log / "peer.stderr.log")
    node = None
    try:
        ready = peer.read()
        assert ready["kind"] == "ready"
        peer.command(kind="actor", generation="1", authority="1")
        node = Host([
            "node", "framework/languages/node/cross-language/session_bind_host.js",
            "tcp://127.0.0.1:0", "mesh", "cross-bind-session-node",
        ], log / "node.stderr.log")
        source = node.read()
        peer.command(kind="configure", rid="cross-bind-session-node", generation=source["generation"],
                     ownerId=source["ownerId"], lease="1", endpoint=source["endpoint"])
        node.command(kind="configure", rid="cross-bind-node", generation=ready["generation"],
                     ownerId=ready["ownerId"], lease="1", endpoint=ready["endpoint"])
        deadline = time.monotonic() + 5
        while node.command(kind="status")["admittedPeerCount"] != 1:
            if time.monotonic() >= deadline:
                raise TimeoutError("Session owner did not admit the Actor owner")
            time.sleep(0.01)
        for session in ("node-first", "node-second"):
            result = node.command(
                kind="bind", rid="cross-bind-node", actorId="cross-bind-actor",
                objectGeneration="1", nodeGeneration=ready["generation"], authority="1",
                lease="1", session=session,
            )
            assert result["kind"] == "terminal", result
            assert result["result"]["terminalResult"] == 0, result
            assert int(result["result"]["streamBinding"]["bindingGeneration"]) > 0, result
            assert result["result"]["streamBinding"]["authorityOwnerGeneration"] == "1", result
            current = peer.command(kind="binding")
            assert current["binding"]["sessionRid"] == session, current
        print(f"PASS Node Session owner -> {language} Actor owner: bind and replacement", flush=True)
    finally:
        if node is not None:
            node.close()
        peer.close()


if __name__ == "__main__":
    for language in ("java", "cpp"):
        verify(language)
