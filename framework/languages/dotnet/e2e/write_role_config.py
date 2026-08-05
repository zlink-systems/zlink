#!/usr/bin/env python3
"""Write one E2E host's IConfiguration document with user-only permissions."""

import json
import os
import stat
import sys


def property_name(option: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in option[2:].split("-"))


COLLECTION_OPTIONS = {
    "ProviderEndpoint": "ProviderEndpoints",
    "RoutePeer": "RoutePeers",
    "RouteServer": "RouteServers",
    "RouteClient": "RouteClients",
}


def main() -> None:
    if len(sys.argv) < 4 or sys.argv[2] != "--":
        raise SystemExit("usage: write_role_config.py OUTPUT -- --key value ...")

    output = sys.argv[1]
    arguments = sys.argv[3:]
    if len(arguments) % 2:
        raise SystemExit("every configuration option requires a value")

    options: dict[str, object] = {}
    for index in range(0, len(arguments), 2):
        option = arguments[index]
        if not option.startswith("--"):
            raise SystemExit(f"invalid configuration option: {option}")
        name = property_name(option)
        value = arguments[index + 1]
        plural = COLLECTION_OPTIONS.get(name, name if name.endswith("s") else f"{name}s")
        if name in COLLECTION_OPTIONS and plural not in options:
            options[plural] = [value]
            continue
        if plural in options and isinstance(options[plural], list):
            options[plural].append(value)
        elif name in options:
            first = options.pop(name)
            options[plural] = [first, value]
        else:
            options[name] = value

    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "w", encoding="utf-8") as file:
        json.dump({"Options": options}, file, indent=2)
    os.chmod(output, stat.S_IRUSR | stat.S_IWUSR)


if __name__ == "__main__":
    main()
