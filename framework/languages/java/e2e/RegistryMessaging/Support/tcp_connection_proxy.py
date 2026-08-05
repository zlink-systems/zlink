#!/usr/bin/env python3
import argparse
import asyncio
from pathlib import Path


async def relay(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        while data := await reader.read(64 * 1024):
            writer.write(data)
            await writer.drain()
    finally:
        writer.close()
        await writer.wait_closed()


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--upstream-port", type=int, required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--ready", required=True)
    options = parser.parse_args()
    evidence = Path(options.evidence)

    async def connected(
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        with evidence.open("a", encoding="utf-8") as output:
            output.write("connection\n")
        deadline = asyncio.get_running_loop().time() + 3
        while True:
            try:
                upstream_reader, upstream_writer = await asyncio.open_connection(
                    "127.0.0.1", options.upstream_port)
                break
            except OSError:
                if asyncio.get_running_loop().time() >= deadline:
                    writer.close()
                    await writer.wait_closed()
                    return
                await asyncio.sleep(0.05)
        await asyncio.gather(
            relay(reader, upstream_writer),
            relay(upstream_reader, writer),
            return_exceptions=True)

    server = await asyncio.start_server(
        connected, "127.0.0.1", options.listen_port)
    Path(options.ready).touch()
    async with server:
        await server.serve_forever()


asyncio.run(main())
