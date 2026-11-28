# SPDX-License-Identifier: Elastic-2.0
"""Observe this cell's ZMP transports and hold A's Framework ADMIT when requested."""

import asyncio
from pathlib import Path
import struct
import sys
import time


def emit(path, run_id, name, **fields):
    line = f"actor-destroy|run={run_id}|event={name}|timeUs={time.time_ns() // 1000}"
    for key, value in fields.items():
        line += f"|{key}={value}"
    with path.open("a", encoding="utf-8") as events:
        events.write(line + "\n")


def destroy_fields(body):
    correlation, = struct.unpack_from(">Q", body, 5)
    actor_end = 14 + body[13]
    generation, = struct.unpack_from(">Q", body, actor_end)
    rid_start = actor_end + 9
    rid_end = rid_start + body[rid_start - 1]
    node, authority, lease = struct.unpack(">QQQ", body[rid_end:])
    return dict(correlation=correlation, flags=body[4], actorId=body[14:actor_end].decode(),
                generation=generation, owner=body[rid_start:rid_end].decode(),
                nodeGeneration=node, ownerGeneration=authority, leaseGeneration=lease)


class Observer:
    def __init__(self, root, control, run_id, hold_admission):
        self.run_id = run_id
        self.paths = {role: root / f"{run_id}.{role}.wire" for role in ("owner", "caller")}
        self.allow = control / f"{run_id}.caller.admission" if hold_admission else None
        self.forwarded_admit = False
        self.correlations = set()
        self.connection_count = 0

    def record(self, role, name, **fields):
        emit(self.paths[role], self.run_id, name, **fields)

    async def relay(self, reader, writer, source, connection, native_requests):
        message = []
        while True:
            header = await reader.readexactly(8)
            magic, version, flags, kind, length = struct.unpack(">BBBBI", header)
            if (magic, version) != (0x5A, 1) or kind > 3:
                raise ValueError("invalid ZMP header")
            extension = await reader.readexactly(8) if kind else b""
            body = await reader.readexactly(length)
            if flags & 0x1E:
                # Core handshake/identity/control, outside the Framework multipart.
                writer.write(header + extension + body)
                await writer.drain()
                continue
            message.append((header + extension, body))
            if flags & 1:
                continue
            first = message[0][1]
            if first.startswith(b"ZM\x01") and len(first) >= 5:
                command = first[3]
                transport_header = message[0][0]
                transport_kind = transport_header[3]
                sequence = transport_header[8:]
                peer = "caller" if source == "owner" else "owner"
                replied_to = None
                if transport_kind == 1:
                    native_requests[(source, sequence)] = command
                elif transport_kind in (2, 3):
                    replied_to = native_requests.pop((peer, sequence), None)
                if command == 2 and source == "owner" and self.allow is not None:
                    if not self.allow.exists():
                        self.record("caller", "admissionHeld", connection=connection,
                                    sender="actor-destroy-owner", action="hold",
                                    reason="admissionCheck")
                        try:
                            while not self.allow.exists():
                                await asyncio.sleep(0.01)
                        except asyncio.CancelledError:
                            self.record("caller", "admissionDisconnected", connection=connection)
                            raise
                    if not self.forwarded_admit:
                        self.forwarded_admit = True
                        self.record("caller", "admissionAllowed")
                if command == 27:
                    fields = destroy_fields(first)
                    self.correlations.add((source, fields["correlation"]))
                    self.record(source, "command27Sent", payloadFrames=len(message) - 1,
                                transportKind=transport_kind, **fields)
                # Other request families may reuse the same Framework correlation.
                # Core's sequence identifies their native replies on this connection.
                if command == 20 and len(first) >= 13 and replied_to in (None, 27):
                    correlation, = struct.unpack_from(">Q", first, 5)
                    if (peer, correlation) in self.correlations:
                        self.record(peer, "command27Reply", correlation=correlation)
            for frame_header, payload in message:
                writer.write(frame_header + payload)
            await writer.drain()
            message.clear()

    async def forward(self, inbound, downstream, destination, port):
        remote = None
        relays = set()
        try:
            upstream, remote = await asyncio.open_connection("127.0.0.2", port)
            peer = "caller" if destination == "owner" else "owner"
            self.connection_count += 1
            connection = self.connection_count
            native_requests = {}
            relays = {
                asyncio.create_task(self.relay(inbound, remote, peer, connection, native_requests)),
                asyncio.create_task(self.relay(upstream, downstream, destination, connection,
                                               native_requests)),
            }
            done, _ = await asyncio.wait(relays, return_when=asyncio.FIRST_COMPLETED)
            for result in done:
                result.result()
        except asyncio.IncompleteReadError as error:
            if error.partial:
                self.record(destination, "error", message="truncated ZMP frame")
        except ConnectionError:
            # Peers may connect before the destination listener starts or close on teardown.
            pass
        except (ValueError, struct.error) as error:
            self.record(destination, "error", message=str(error))
        finally:
            for task in relays:
                task.cancel()
            await asyncio.gather(*relays, return_exceptions=True)
            if remote is not None:
                remote.close()
            downstream.close()


async def run(root, run_id, scenario, owner_port, caller_port, control):
    observer = Observer(root, control, run_id, scenario == "admissionCheck")
    async with (
        await asyncio.start_server(
            lambda reader, writer: observer.forward(reader, writer, "owner", owner_port),
            "127.0.0.1", owner_port) as owner,
        await asyncio.start_server(
            lambda reader, writer: observer.forward(reader, writer, "caller", caller_port),
            "127.0.0.1", caller_port) as caller,
    ):
        for role in observer.paths:
            observer.record(role, "wireObserved")
        (root / f"{run_id}.wire.ready").touch()
        await asyncio.gather(owner.serve_forever(), caller.serve_forever())


if __name__ == "__main__":
    root, run_id, scenario, owner_port, caller_port, control = sys.argv[1:]
    asyncio.run(run(Path(root), run_id, scenario, int(owner_port), int(caller_port),
                    Path(control)))
