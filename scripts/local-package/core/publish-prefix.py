#!/usr/bin/env python3
"""Publish a verified Core stage under its exact provenance digest.

Both local builds and releases use this immutable namespace. Rename publishes
all files together; an identical concurrent publisher reuses the existing entry.
The caller owns staging cleanup, including an unpublished duplicate stage.
"""
import errno
import hashlib
import os
from pathlib import Path
import sys


def publish(stage, cache):
    manifest = Path('share/zlink/core-package-provenance.json')
    provenance = (stage / manifest).read_bytes()
    prefix = cache / hashlib.sha256(provenance).hexdigest()
    cache.mkdir(parents=True, exist_ok=True)
    try:
        os.rename(stage, prefix)
    except OSError as error:
        if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
            raise
        # An existing destination must contain this exact provenance.
        if (prefix / manifest).read_bytes() != provenance:
            raise RuntimeError(f'Core cache provenance collision: {prefix}') from error
    return prefix


if __name__ == '__main__':
    print(publish(Path(sys.argv[1]), Path(sys.argv[2])))
