#!/usr/bin/env python3

import argparse
import asyncio
import contextlib
import pathlib
import time


async def copy_stream(reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
    try:
        while data := await reader.read(64 * 1024):
            writer.write(data)
            await writer.drain()
    finally:
        with contextlib.suppress(Exception):
            writer.close()
            await writer.wait_closed()


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--upstream-port", type=int, required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True)
    parser.add_argument("--ready", type=pathlib.Path, required=True)
    options = parser.parse_args()

    async def handle(reader: asyncio.StreamReader,
                     writer: asyncio.StreamWriter) -> None:
        with options.evidence.open("a", encoding="utf-8") as evidence:
            evidence.write(f"{time.monotonic_ns()}\n")
        while True:
            try:
                upstream_reader, upstream_writer = (
                    await asyncio.open_connection(
                        "127.0.0.1", options.upstream_port))
                break
            except OSError:
                await asyncio.sleep(0.05)
        await asyncio.gather(
            copy_stream(reader, upstream_writer),
            copy_stream(upstream_reader, writer))

    server = await asyncio.start_server(
        handle, "127.0.0.1", options.listen_port)
    options.ready.write_text("ready\n", encoding="utf-8")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
