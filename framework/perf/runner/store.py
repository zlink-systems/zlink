"""The run's Docker Redis (§20): started before the first cell that needs a Store, kept between cells, and removed
by its exact container ID when the run ends or is interrupted. Host Redis is never used."""
from __future__ import annotations

import json
from pathlib import Path
import re
import socket
import subprocess
import time

IMAGE = "redis:7-alpine"
CONTAINER_ID = re.compile(r"^[0-9a-f]{12,64}$")
LAUNCH_TIMEOUT_SECONDS = 300
READINESS_TIMEOUT_SECONDS = 60
READINESS_POLL_SECONDS = 0.5
READINESS_PROBE_TIMEOUT_SECONDS = 5


def docker(*command: str, timeout: float = 30) -> str:
    result = subprocess.run(["docker", *command], capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"docker {command[0]} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout.strip()


def check_available() -> None:
    """Preflight: the Docker daemon must answer, or a Store cell cannot run."""
    try:
        docker("info", "--format", "{{.ServerVersion}}", timeout=15)
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        raise ValueError("Docker is required for a scenario that needs a Store: " + str(error)) from error


class RunStore:
    def __init__(self, run_id: str, output: Path):
        self.run_id = run_id
        self.output = output
        self.info: dict | None = None
        self.removal: dict | None = None

    @property
    def container_id(self) -> str | None:
        return self.info["containerId"] if self.info else None

    def acquire(self) -> dict:
        """The run's Redis; the first call starts it. The host port is chosen by Docker, so no port race exists."""
        if self.info is not None:
            return self.info
        host_address = "127.0.0.1"
        container_id = docker("run", "-d", "--name", f"zlink-perf-{self.run_id}", "--tmpfs", "/data",
                              "-p", f"{host_address}::6379", IMAGE, timeout=LAUNCH_TIMEOUT_SECONDS)
        if not CONTAINER_ID.match(container_id):
            raise RuntimeError("docker run did not return a container ID: " + container_id)
        self.info = {"containerId": container_id}  # from here on release() removes it even if readiness fails
        host_port = docker("port", container_id, "6379/tcp").splitlines()[0].rsplit(":", 1)[1]
        deadline = time.monotonic() + READINESS_TIMEOUT_SECONDS
        while True:
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
                    connection.settimeout(READINESS_PROBE_TIMEOUT_SECONDS)
                    if connection.connect_ex((host_address, int(host_port))) == 0 \
                            and docker("exec", container_id, "redis-cli", "ping",
                                       timeout=READINESS_PROBE_TIMEOUT_SECONDS) == "PONG":
                        break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Redis published endpoint did not become ready within "
                                   f"{READINESS_TIMEOUT_SECONDS}s: " + container_id)
            time.sleep(READINESS_POLL_SECONDS)
        image = json.loads(docker("image", "inspect", IMAGE, "--format", "{{json .}}"))
        self.info = {"provider": "redis", "containerId": container_id, "image": IMAGE, "imageId": image["Id"],
                     "repoDigests": image["RepoDigests"], "hostPort": int(host_port), "endpoint": host_address + ":" + host_port,
                     "version": docker("exec", container_id, "redis-server", "--version")}
        return self.info

    def config(self, config_hash: str) -> dict:
        """The Store section of a role config; every cell gets its own namespace and never reuses one."""
        info = self.acquire()
        return {"provider": info["provider"], "endpoint": info["endpoint"], "containerId": info["containerId"],
                "image": info["image"], "imageDigest": next(iter(info["repoDigests"]), info["imageId"]),
                "namespace": f"zlink-perf:{self.run_id}:{config_hash[:12]}"}

    def release(self) -> None:
        if self.info is None or self.removal is not None:
            return
        container_id = self.info["containerId"]
        try:
            docker("rm", "-fv", container_id, timeout=20)
            self.removal = {"containerId": container_id, "removed": True}
        except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
            self.removal = {"containerId": container_id, "removed": False, "error": str(error)}
        (self.output / "store.json").write_text(json.dumps({**self.info, "removal": self.removal}, indent=2) + "\n")
