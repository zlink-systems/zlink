#!/usr/bin/env python3
"""Validate the complete schema plus the invariants JSON Schema cannot express."""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import sys
import jsonschema
from results import CATALOG, BOUNDS, MAX_U64, u64, decimal, histogram_merge, ranges, intersection_count

SCHEMA = json.loads((Path(__file__).resolve().parent / 'schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(SCHEMA)


def i64(value):
    if not isinstance(value, str) or not value.isascii() or value == "-0":
        raise ValueError("SchemaMismatch: noncanonical I64")
    try: number = int(value)
    except ValueError: raise ValueError("SchemaMismatch: noncanonical I64")
    if str(number) != value or not -(1 << 63) <= number < (1 << 63):
        raise ValueError("SchemaMismatch: noncanonical or overflowing I64")
    return number


def exact_integers(value, schema):
    reference = schema.get("$ref")
    if reference:
        name = reference.rsplit("/", 1)[-1]
        if name == "u64": u64(value)
        elif name == "i64": i64(value)
        elif name == "decimal": decimal(value)
        else: exact_integers(value, SCHEMA["$defs"][name])
        return
    for union in ("anyOf", "oneOf"):
        if union in schema:
            for candidate in schema[union]:
                resolver = jsonschema.RefResolver.from_schema(SCHEMA)
                if jsonschema.Draft202012Validator(candidate, resolver=resolver).is_valid(value):
                    exact_integers(value, candidate)
                    break
            return
    if isinstance(value, dict):
        for key, item in value.items():
            candidate = schema.get("properties", {}).get(key, schema.get("additionalProperties"))
            if isinstance(candidate, dict): exact_integers(item, candidate)
    elif isinstance(value, list) and isinstance(schema.get("items"), dict):
        for item in value: exact_integers(item, schema["items"])


def validate_schema(value, definition=None):
    document = SCHEMA if definition is None else {**SCHEMA, 'oneOf': [{'$ref': '#/$defs/' + definition}]}
    errors = sorted(jsonschema.Draft202012Validator(document).iter_errors(value), key=lambda error: str(list(error.path)))
    if errors:
        item = errors[0]
        pointer = '/' + '/'.join(map(str, item.absolute_path))
        # A top-level oneOf error hides the useful missing-field diagnosis.
        if item.context:
            item = min(item.context, key=lambda error: len(error.schema_path))
            pointer = '/' + '/'.join(map(str, item.absolute_path))
        raise ValueError('SchemaMismatch: ' + pointer + ': ' + item.message)
    exact_integers(value, document)


def pointer_escape(value):
    return str(value).replace('~', '~0').replace('/', '~1')


def check_nulls(value, reasons, pointer='', excluded=()):
    if value is None:
        if not any(pointer.startswith(prefix) for prefix in excluded) and pointer not in reasons:
            raise ValueError('SchemaMismatch: null has no reason at ' + pointer)
    elif isinstance(value, dict):
        for key, item in value.items():
            if key not in ('nullReasons', 'provenance', 'runtimeMetrics', 'publicStatus', 'publicMetrics'):
                check_nulls(item, reasons, pointer + '/' + pointer_escape(key), excluded)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            if pointer == '/timeSeries':
                check_nulls(item, item.get('nullReasons', {}), '', excluded)
            else:
                check_nulls(item, reasons, pointer + '/' + str(index), excluded)


def finite_values(value):
    if isinstance(value, float) and not math.isfinite(value): raise ValueError('SchemaMismatch: nonfinite JSON number')
    if isinstance(value, dict):
        for item in value.values(): finite_values(item)
    elif isinstance(value, list):
        for item in value: finite_values(item)


def check_metrics(value):
    metrics, nulls = value['metrics'], value['nullReasons']
    for key in CATALOG['counts']:
        if metrics.get(key) is not None: u64(metrics[key])
    for family in CATALOG['errorNamespaces']:
        for item in metrics[family].values(): u64(item)
    for key, histogram in value['histograms'].items():
        if histogram is not None:
            histogram_merge([histogram])
            prefix = CATALOG['histogramPrefixes'].get(key)
            if prefix and u64(histogram['count']) < 100000:
                pointer = '/metrics/' + prefix + '.p999Ms'
                if metrics.get(prefix + '.p999Ms') is not None or nulls.get(pointer, {}).get('code') != 'INSUFFICIENT_SAMPLES':
                    raise ValueError('SchemaMismatch: p999 with fewer than 100000 samples needs INSUFFICIENT_SAMPLES')


def validate_snapshot(value, config=None):
    validate_schema(value, 'PerfMetricsSnapshot')
    finite_values(value)
    check_nulls(value, value['nullReasons'])
    check_metrics(value)
    u64(value['resetSeq'])
    if config:
        for key in ('runId', 'cellId', 'configHash', 'comparisonKey', 'language'):
            if value[key] != config[key]: raise ValueError('SchemaMismatch: original ' + key + ' differs from cell config')
    window = value['window']
    if value['phase']=='complete' and any(window[key] is None for key in ('startedAtUnixMs','endedAtUnixMs','startTicks','endTicks','measuredSeconds','settleSeconds')):
        raise ValueError('SchemaMismatch: completed application snapshot requires an actually completed window and settle observation')
    if window['measuredSeconds'] is not None:
        start, end = i64(window['startTicks']), i64(window['endTicks'])
        if end <= start or not math.isclose(window['measuredSeconds'], (end - start) / 1e9, rel_tol=0, abs_tol=1e-9):
            raise ValueError('SchemaMismatch: monotonic window disagrees with measuredSeconds')
        if window['settleSeconds'] is not None and window['settleSeconds'] < 0:
            raise ValueError('SchemaMismatch: negative settle span')
    intervals = value['timeSeries']
    if value['phase'] == 'complete' and window['measuredSeconds'] is not None:
        expected_ms = window['measuredSeconds'] * 1000
        offset = 0.0
        for index, item in enumerate(intervals):
            if not math.isclose(item['offsetMs'], offset, abs_tol=1e-6) or item['durationMs'] <= 0 or item['durationMs'] > 100 + 1e-6:
                raise ValueError('SchemaMismatch: timeSeries must have dense 100ms bins and final partial bin')
            if index + 1 < len(intervals) and not math.isclose(item['durationMs'], 100, abs_tol=1e-6):
                raise ValueError('SchemaMismatch: only final timeSeries bin may be partial')
            offset += item['durationMs']
            for count in item['counts'].values(): u64(count)
        if not math.isclose(offset, expected_ms, abs_tol=1e-6):
            raise ValueError('SchemaMismatch: timeSeries coverage differs from owner window')
    for key in ('messages.sent', 'messages.completed', 'slo.eligible'):
        if intervals and value['metrics'].get(key) is not None:
            if sum(u64(item['counts'].get(key, '0')) for item in intervals) != u64(value['metrics'][key]):
                raise ValueError('SchemaMismatch: timeSeries count sum differs from ' + key)
    if value['phase'] == 'complete' and intervals:
        observation = value['runtimeMetrics'].get('cpuSamples')
        if not observation or observation.get('type') != 'array' or not isinstance(observation.get('value'), list):
            raise ValueError('SchemaMismatch: completed window requires original CPU sample evidence')
        spans = [[] for _ in intervals]
        previous_end = None
        for sample in observation['value']:
            if set(sample) != {'binIndex','startOffsetMs','endOffsetMs','observedDurationNs','cpuDeltaNs'}:
                raise ValueError('SchemaMismatch: CPU sample evidence fields differ')
            start_ms, end_ms = sample['startOffsetMs'], sample['endOffsetMs']
            duration, delta = u64(sample['observedDurationNs']), u64(sample['cpuDeltaNs'])
            if not isinstance(start_ms, (int,float)) or not isinstance(end_ms, (int,float)) or not math.isfinite(start_ms) or not math.isfinite(end_ms):
                raise ValueError('SchemaMismatch: CPU offsets must be finite numbers')
            if start_ms < 0 or start_ms >= window['measuredSeconds'] * 1000 or end_ms <= start_ms or duration == 0:
                raise ValueError('SchemaMismatch: CPU actual sample span is outside window or nonpositive')
            if not math.isclose((end_ms-start_ms)*1e6,duration,rel_tol=0,abs_tol=1):
                raise ValueError('SchemaMismatch: CPU actual offsets disagree with observed duration')
            index = sample['binIndex']
            if type(index) is not int or index != math.floor(start_ms/100) or index >= len(spans):
                raise ValueError('SchemaMismatch: CPU samples must be assigned by actual span start')
            if previous_end is not None and not math.isclose(start_ms,previous_end,abs_tol=1e-6):
                raise ValueError('SchemaMismatch: CPU original spans must be contiguous without interpolation')
            previous_end = end_ms
            spans[index].append((duration,delta))
        for item, assigned in zip(intervals,spans):
            if assigned:
                expected = 100 * sum(delta for duration,delta in assigned) / sum(duration for duration,delta in assigned)
                if item['cpuPercent'] is None or not math.isclose(item['cpuPercent'],expected,rel_tol=1e-9,abs_tol=1e-9):
                    raise ValueError('SchemaMismatch: bin CPU differs from time-weighted actual sample spans')
            elif item['cpuPercent'] is not None or item['nullReasons'].get('/cpuPercent',{}).get('code') != 'NO_SAMPLES':
                raise ValueError('SchemaMismatch: empty CPU bins must remain null with NO_SAMPLES')
    for key in ('sourceEvidence', 'deliveryEvidence'):
        item = value['runtimeMetrics'].get(key)
        if item and item.get('value') is not None:
            seen = set()
            for stream in item['value']:
                if stream['clientId'] in seen: raise ValueError('SchemaMismatch: duplicate evidence clientId')
                seen.add(stream['clientId'])
                if key == 'sourceEvidence':
                    names = ('attemptedRanges', 'windowAdmissionRanges', 'settleAdmissionRanges')
                    for name in names: ranges(stream[name])
                    for name in names[1:]:
                        if intersection_count(stream[name], stream['attemptedRanges']) != sum(b-a+1 for a,b in ranges(stream[name])):
                            raise ValueError('SchemaMismatch: admission absent from attempted sequence cohort')
                    if intersection_count(stream[names[1]], stream[names[2]]): raise ValueError('SchemaMismatch: admission windows overlap')
                else:
                    ranges(stream['windowRanges']); ranges(stream['settleRanges']); u64(stream['duplicateCount'])
                    if intersection_count(stream['windowRanges'], stream['settleRanges']): raise ValueError('SchemaMismatch: receipt windows overlap')
    for key, definition in (('publisherSequences','PublisherSequences'), ('subscriberSequences','SubscriberSequences')):
        item = value['runtimeMetrics'].get(key)
        if item and item.get('value') is not None:
            validate_schema(item['value'], definition)
            for field, sequence in item['value'].items():
                if field.endswith('Ranges'): ranges(sequence)
            for identity in ('runId', 'cellId', 'resetSeq'):
                if item['value'][identity] != value[identity]: raise ValueError('SchemaMismatch: sequence evidence identity differs')


def validate_result(value):
    validate_schema(value, 'PerfResult')
    finite_values(value)
    check_nulls(value, value['nullReasons'], excluded=('/processes/', '/ownerWindows/'))
    check_metrics(value)
    if value['status'] != 'valid' and value['baselineEligible']: raise ValueError('SchemaMismatch: unsuccessful cell baselineEligible')
    if len(value['metricOwners']) != len(set(value['metricOwners'])): raise ValueError('SchemaMismatch: duplicate metric owner')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', type=Path)
    parser.add_argument('--definition', choices=list(SCHEMA['$defs']))
    args = parser.parse_args(argv)
    failed = False
    for path in args.files:
        try:
            value = json.loads(path.read_text())
            if args.definition: validate_schema(value, args.definition)
            elif 'status' in value: validate_result(value)
            else: validate_snapshot(value)
            print(str(path) + ': valid')
        except (OSError, ValueError, KeyError, TypeError) as error:
            failed = True
            print(str(path) + ': ' + str(error), file=sys.stderr)
    return int(failed)

if __name__ == '__main__': sys.exit(main())
