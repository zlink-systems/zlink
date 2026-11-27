# SPDX-License-Identifier: LicenseRef-Zlink
"""Validate fixture payloads against the local wire definition."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate property: {key}')
        result[key] = value
    return result


def check(value, kind, definition, path):
    if kind.endswith('?'):
        if value is None:
            return
        return check(value, kind[:-1], definition, path)
    if kind.endswith('[]'):
        if not isinstance(value, list):
            raise ValueError(f'{path}: expected array')
        for index, item in enumerate(value):
            check(item, kind[:-2], definition, f'{path}[{index}]')
    elif kind in definition['objects']:
        if not isinstance(value, dict):
            raise ValueError(f'{path}: expected object')
        for name, field_type in definition['objects'][kind].items():
            if name not in value:
                raise ValueError(f'{path}.{name}: required')
            check(value[name], field_type, definition, f'{path}.{name}')
    elif kind in definition['enums']:
        if value not in definition['enums'][kind]:
            raise ValueError(f'{path}: expected a declared {kind} name as a JSON string')
    elif kind in ('u64', 'positive_u64', 'i64'):
        pattern = r'(?:0|[1-9][0-9]*)' if kind != 'i64' else r'(?:0|-?[1-9][0-9]*)'
        if not isinstance(value, str) or not re.fullmatch(pattern, value):
            raise ValueError(f'{path}: expected canonical decimal string')
        number = int(value)
        lower, upper = (-(2**63), 2**63 - 1) if kind == 'i64' else (0, 2**64 - 1)
        if not lower <= number <= upper or (kind == 'positive_u64' and number == 0):
            raise ValueError(f'{path}: out of range')
    elif kind == 'string':
        if not isinstance(value, str):
            raise ValueError(f'{path}: expected string')
    elif kind == 'bool':
        if not isinstance(value, bool):
            raise ValueError(f'{path}: expected boolean')
    elif kind != 'json':
        raise ValueError(f'unknown definition type: {kind}')


def main():
    definition = json.loads((ROOT / 'wire-v1.json').read_text(encoding='utf-8'), object_pairs_hook=unique)
    vectors = json.loads((ROOT / 'fixtures.json').read_text(encoding='utf-8'), object_pairs_hook=unique)
    failures = 0
    for vector in vectors:
        try:
            packet = vector['packet']
            if packet not in definition['packets']:
                raise ValueError(f'unknown packet name: {packet}')
            payload = json.loads(vector['payload'], object_pairs_hook=unique,
                                 parse_constant=lambda x: (_ for _ in ()).throw(ValueError(f'invalid JSON: {x}')))
            check(payload, definition['packets'][packet], definition, '$')
            error = None
        except (ValueError, TypeError) as exc:
            error = str(exc)
        passed = (error is None) == vector['valid']
        failures += not passed
        print(f'{"PASS" if passed else "FAIL"} {vector["name"]}: {"accepted" if error is None else "rejected: " + error}')
    print(f'{len(vectors) - failures}/{len(vectors)} passed')
    return bool(failures)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as exc:
        print(f'FAIL definition or fixture: {exc}', file=sys.stderr)
        sys.exit(1)
