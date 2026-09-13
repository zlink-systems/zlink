"""Schema 3 aggregation of preserved application observations (never percentile averages)."""
from __future__ import annotations
import copy
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
BOUNDS = json.loads((HERE / 'histogram-bounds-ns.json').read_text())
CATALOG = json.loads((HERE / 'metric-catalog.json').read_text())
MAX_U64 = (1 << 64) - 1
OUTCOMES = tuple(CATALOG['outcomes'])


def u64(value):
    if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or str(int(value)) != value or int(value) > MAX_U64:
        raise ValueError('SchemaMismatch: noncanonical or overflowing U64')
    return int(value)


def count_text(value):
    if not isinstance(value, int) or not 0 <= value <= MAX_U64:
        raise ValueError('CounterOverflow: aggregate count exceeds U64')
    return str(value)


def decimal(value):
    if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or str(int(value)) != value:
        raise ValueError('SchemaMismatch: noncanonical decimal text')
    return int(value)


def reason(code, text, **extra):
    return {'code': code, 'reason': text, 'owner': 'perf/README.ko.md', **extra}


def write_json(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write('\n')


def ranges(value):
    result = []
    previous = -2
    for pair in value:
        if not isinstance(pair, dict) or set(pair) != {'first', 'last'}:
            raise ValueError('SchemaMismatch: sequence range must be an inclusive pair')
        first, last = u64(pair['first']), u64(pair['last'])
        if first > last or first <= previous + 1:
            raise ValueError('SchemaMismatch: sequence ranges must be maximal, disjoint and sorted')
        result.append((first, last))
        previous = last
    return result


def range_count(value):
    return sum(last - first + 1 for first, last in ranges(value))


def intersection_count(left, right):
    a, b = ranges(left), ranges(right)
    i = j = total = 0
    while i < len(a) and j < len(b):
        total += max(0, min(a[i][1], b[j][1]) - max(a[i][0], b[j][0]) + 1)
        if a[i][1] < b[j][1]:
            i += 1
        else:
            j += 1
    return total


def intersect_ranges(left, right):
    a, b = ranges(left), ranges(right)
    i = j = 0
    output = []
    while i < len(a) and j < len(b):
        first, last = max(a[i][0], b[j][0]), min(a[i][1], b[j][1])
        if first <= last:
            output.append({'first': str(first), 'last': str(last)})
        if a[i][1] < b[j][1]: i += 1
        else: j += 1
    return output


def histogram_merge(values):
    if not values:
        raise ValueError('CollectionFailure: no histogram owners')
    result = {'unit': 'ms', 'ticksUnit': 'ns', 'bucketSpec': 'ns-1us-1pct-60s-v1',
              'boundsNs': list(map(str, BOUNDS)), 'counts': ['0'] * len(BOUNDS), 'overflow': '0',
              'count': '0', 'sumNs': '0', 'maxNs': None,
              'percentileMethod': 'nearest-rank-bucket-upper-bound'}
    for value in values:
        if any(value.get(key) != result[key] for key in ('unit', 'ticksUnit', 'bucketSpec', 'boundsNs', 'percentileMethod')):
            raise ValueError('SchemaMismatch: histogram contract differs')
        if len(value['counts']) != len(BOUNDS) or sum(map(u64, value['counts'])) + u64(value['overflow']) != u64(value['count']):
            raise ValueError('SchemaMismatch: histogram count does not reconcile')
        n = u64(value['count'])
        total = decimal(value['sumNs'])
        maximum = None if value['maxNs'] is None else u64(value['maxNs'])
        if (n == 0) != (maximum is None) or (n == 0 and total) or (n and total > n * maximum):
            raise ValueError('SchemaMismatch: histogram sum/max/count disagree')
        result['counts'] = [count_text(u64(a) + u64(b)) for a, b in zip(result['counts'], value['counts'])]
        for key in ('count', 'overflow'): result[key] = count_text(u64(result[key]) + u64(value[key]))
        result['sumNs'] = str(int(result['sumNs']) + total)
        if maximum is not None: result['maxNs'] = str(max(maximum, int(result['maxNs'] or '0')))
    return result


def export_latency(histogram, prefix, histogram_key, metrics, reasons):
    n = u64(histogram['count'])
    for suffix in CATALOG['latencySuffixes']:
        pointer = '/metrics/' + prefix + '.' + suffix
        reasons.pop(pointer, None)
        value = None
        code = 'NO_SAMPLES'
        if suffix == 'p999Ms' and n < 100000:
            code = 'INSUFFICIENT_SAMPLES'
        elif n:
            if suffix == 'meanMs': value = int(histogram['sumNs']) / n / 1e6
            elif suffix == 'maxMs': value = u64(histogram['maxNs']) / 1e6
            else:
                numerator, denominator = {'p50Ms': (50,100), 'p95Ms': (95,100), 'p99Ms': (99,100), 'p999Ms': (999,1000)}[suffix]
                rank = (numerator * n + denominator - 1) // denominator
                cumulative = 0
                for bound, bucket in zip(BOUNDS, histogram['counts']):
                    cumulative += u64(bucket)
                    if cumulative >= rank:
                        value = bound / 1e6
                        break
                if value is None: code = 'HISTOGRAM_OVERFLOW'
        metrics[prefix + '.' + suffix] = value
        if value is None:
            reasons[pointer] = reason(code, 'No successful samples.' if code == 'NO_SAMPLES' else
                                     'p999 requires at least 100000 samples.' if code == 'INSUFFICIENT_SAMPLES' else
                                     'Nearest rank exceeds the final finite histogram bucket.',
                                     **({'lowerBoundMs': 60000} if code == 'HISTOGRAM_OVERFLOW' else {}))
    pointer = '/histograms/' + histogram_key + '/maxNs'
    reasons.pop(pointer, None)
    if not n: reasons[pointer] = reason('NO_SAMPLES', 'No successful samples.')


def metric_defaults():
    keys = set(CATALOG['counts'] + CATALOG['rates'] + CATALOG['unsupported'] + CATALOG['errorNamespaces'])
    keys.update(prefix + '.' + suffix for prefix in CATALOG['latencyPrefixes'] for suffix in CATALOG['latencySuffixes'])
    return {key: {} if key in CATALOG['errorNamespaces'] else None for key in sorted(keys)}


def evidence(snapshot, key):
    item = snapshot['runtimeMetrics'].get(key)
    if not isinstance(item, dict) or 'value' not in item or item['value'] is None:
        raise ValueError('CollectionFailure: required ' + key + ' observation is absent')
    return item['value']


def aggregate(cell, config, client_files, server_files, issues):
    from validator import validate_snapshot
    cell = Path(cell)
    issues = list(issues)
    originals = {}
    templates = {}
    for name in client_files + server_files:
        try:
            value = json.loads((cell / name).read_text())
            templates[name] = value
            validate_snapshot(value, config)
            if value['resetSeq'] != '1' or value['phase'] != 'complete':
                raise ValueError('PhaseMismatch: original has no completed measured reset epoch')
            originals[name] = value
        except (OSError, KeyError, ValueError, TypeError) as error:
            issues.append({'code': 'SchemaMismatch' if 'SchemaMismatch' in str(error) else 'CollectionFailure',
                           'message': str(error), 'sourceFile': name})
    owner_names = config.get('metricOwners', client_files if config['scenario'].startswith('cs-') or config['scenario'] == 'session-echo-only' else [name for name in server_files if templates.get(name, {}).get('provenance', {}).get('source')])
    if not owner_names and server_files: owner_names = [server_files[-1]]
    selected = [originals[name] for name in owner_names if name in originals]
    if len(selected) != len(owner_names): issues.append({'code': 'CollectionFailure', 'message': 'Missing primary owner original.', 'sourceFile': ','.join(owner_names)})
    metrics = metric_defaults()
    histograms = {key: None for key in CATALOG['histogramPrefixes']}
    nulls = {'/metrics/' + key: reason('COLLECTION_FAILED', 'No measured aggregate observation.') for key, value in metrics.items() if value is None}
    nulls.update({'/histograms/' + key: reason('COLLECTION_FAILED', 'No measured aggregate histogram.') for key in histograms})
    participants = list(originals.values())
    one_way, publish = config.get('mode') == 'send', config.get('mode') == 'publish'
    try:
        if selected:
            metrics.update(copy.deepcopy(selected[0]['metrics']))
            nulls.update({key: copy.deepcopy(value) for key, value in selected[0]['nullReasons'].items() if key.startswith(('/metrics/', '/histograms/'))})
            for key in CATALOG['counts']:
                values = [item['metrics'].get(key) for item in selected]
                if all(value is not None for value in values): metrics[key] = count_text(sum(map(u64, values)))
            for family in CATALOG['errorNamespaces']:
                combined = {}
                for item in selected:
                    for key, count in item['metrics'][family].items(): combined[key] = count_text(u64(combined.get(key, '0')) + u64(count))
                metrics[family] = combined
            for key, prefix in CATALOG['histogramPrefixes'].items():
                values = [item['histograms'].get(key) for item in selected]
                if all(value is not None for value in values):
                    histograms[key] = histogram_merge(values)
                    export_latency(histograms[key], prefix, key, metrics, nulls)
                else:
                    histograms[key] = None
                    nulls['/histograms/' + key] = copy.deepcopy(selected[0]['nullReasons'].get('/histograms/' + key, reason('NOT_APPLICABLE', 'This operation has no histogram of this kind.')))
            for direction in ('request', 'send', 'reply', 'event'):
                for family in ('applicationMessages', 'applicationPayloadBytes'):
                    key = family + '.' + direction
                    metrics[key] = count_text(sum(u64(item['metrics'][key]) for item in participants))
            for key in ('throughput.messagesPerSec', 'throughput.megabytesPerSec'):
                metrics[key] = sum(item['metrics'][key] for item in participants)
            if not one_way and not publish:
                for item in selected:
                    counts = {key: u64(item['metrics']['messages.' + key]) for key in OUTCOMES}
                    if counts['sent'] != sum(count for key, count in counts.items() if key != 'sent'):
                        raise ValueError('SchemaMismatch: echo terminal cohort does not reconcile')
                    for hist, count in (('latencyMs', 'completed'), ('settleLatencyMs', 'settleCompleted')):
                        if u64(item['histograms'][hist]['count']) != counts[count]: raise ValueError('SchemaMismatch: echo histogram count differs')
                metrics['throughput.kops'] = sum(u64(item['metrics']['messages.completed']) / item['window']['measuredSeconds'] / 1000 for item in selected)
                for key in ('failed', 'timeout', 'cancelled', 'unresolved'):
                    if u64(metrics['messages.' + key]): issues.append({'code': 'EchoOutcomeFailure', 'message': key + '=' + metrics['messages.' + key], 'sourceFile': ','.join(owner_names)})
                eligible = u64(metrics['slo.eligible'])
                met = u64(metrics['slo.met'])
                if met > eligible: raise ValueError('SchemaMismatch: SLO met exceeds eligible')
                metrics['slo.missed'] = count_text(eligible - met)
                metrics['slo.missRatio'] = (eligible - met) / eligible if eligible else None
                metrics['slo.goodputOpsPerSec'] = sum(item['metrics']['slo.goodputOpsPerSec'] for item in selected)
                if not eligible: nulls['/metrics/slo.missRatio'] = reason('ZERO_DENOMINATOR', 'No eligible operations.')
            elif one_way:
                source = selected[0]
                if u64(source['metrics']['messages.sent']) != sum(u64(source['metrics']['messages.' + key]) for key in ('admitted','failed','timeout','cancelled','unresolved')):
                    raise ValueError('SchemaMismatch: one-way admission cohort does not reconcile')
                admissions = evidence(source, 'sourceEvidence')
                receiver_names = config.get('receiverFiles', [name for name in server_files if name not in owner_names])
                receivers = [originals[name] for name in receiver_names]
                streams = {item['clientId']: item for item in admissions}
                admitted = sum(range_count(item['windowAdmissionRanges']) for item in admissions)
                attempted = sum(range_count(item['attemptedRanges']) for item in admissions)
                all_admitted = admitted + sum(range_count(item['settleAdmissionRanges']) for item in admissions)
                if attempted != u64(source['metrics']['messages.sent']) or all_admitted != u64(source['metrics']['messages.admitted']):raise ValueError('SchemaMismatch: source evidence differs from admission counts')
                delivered_window = delivered_settle = duplicates = 0
                receiver_rates = 0.0
                for receiver in receivers:
                    own_window = 0
                    for item in evidence(receiver, 'deliveryEvidence'):
                        if item['clientId'] not in streams: continue
                        admitted_ranges = streams[item['clientId']]['windowAdmissionRanges']
                        own_window += intersection_count(admitted_ranges, item['windowRanges'])
                        delivered_settle += intersection_count(admitted_ranges, item['settleRanges'])
                        if intersection_count(item['windowRanges'], item['settleRanges']): raise ValueError('SchemaMismatch: receiver window and settle receipt overlap')
                        duplicates += u64(item['duplicateCount'])
                    delivered_window += own_window
                    receiver_rates += own_window / receiver['window']['measuredSeconds']
                delivered = delivered_window + delivered_settle
                if delivered > admitted: raise ValueError('SchemaMismatch: one-way deliveries exceed admissions')
                metrics.update({'messages.admittedInWindow': count_text(admitted), 'send.deliveredInWindow': count_text(delivered_window),
                                'send.settleDelivered': count_text(delivered_settle), 'send.delivered': count_text(delivered),
                                'send.duplicateEvents': count_text(duplicates), 'send.admissionOpsPerSec': admitted / source['window']['measuredSeconds'],
                                'send.deliveryOpsPerSec': receiver_rates, 'send.deliveryRatio': delivered / admitted if admitted else None})
                if not admitted:
                    nulls['/metrics/send.deliveryRatio'] = reason('ZERO_DENOMINATOR', 'No measured source admission.')
                    issues.append({'code': 'NoAdmission', 'message': 'No measured admission evidence.', 'sourceFile': ','.join(owner_names)})
                if delivered != admitted or duplicates: issues.append({'code': 'OneWayDeliveryFailure', 'message': 'Delivery ratio must be 1 and duplicates 0.', 'sourceFile': ','.join(receiver_names)})
            else:
                source = selected[0]
                publisher = evidence(source, 'publisherSequences')
                admitted = range_count(publisher['windowSuccessRanges'])
                all_published = admitted + range_count(publisher['settleSuccessRanges'])
                if all_published != u64(source['metrics']['messages.published']) or admitted != u64(source['metrics']['messages.publishedInWindow']):raise ValueError('SchemaMismatch: publisher evidence differs from public admission counters')
                if range_count(publisher['attemptedRanges']) != u64(source['metrics']['messages.sent']) or u64(source['metrics']['messages.sent']) != all_published + sum(u64(source['metrics']['messages.'+key]) for key in ('failed','timeout','cancelled','unresolved')):raise ValueError('SchemaMismatch: publish cohort does not reconcile')
                receiver_names = config.get('receiverFiles', [name for name in server_files if name not in owner_names])
                subscribers = [originals[name] for name in receiver_names]
                window_total = settle_total = duplicates = 0
                delivery_rate = 0.0
                expected_subscribers = config['workload']['subscriberCount']
                ratios = []
                out_of_cohort = 0
                ids = set()
                for subscriber in subscribers:
                    item = evidence(subscriber, 'subscriberSequences')
                    if item['subscriberId'] in ids: raise ValueError('SchemaMismatch: duplicate subscriberId')
                    ids.add(item['subscriberId'])
                    window = intersection_count(publisher['windowSuccessRanges'], item['windowRanges'])
                    settle = intersection_count(publisher['windowSuccessRanges'], item['settleRanges'])
                    if intersection_count(item['windowRanges'], item['settleRanges']): raise ValueError('SchemaMismatch: subscriber window and settle overlap')
                    ratios.append((window + settle) / admitted if admitted else 0)
                    out_of_cohort += range_count(item['windowRanges']) + range_count(item['settleRanges']) - window - settle
                    window_total += window
                    settle_total += settle
                    duplicates += u64(item['duplicateEvents'])
                    delivery_rate += window / subscriber['window']['measuredSeconds']
                if len(ids) != expected_subscribers: raise ValueError('CollectionFailure: subscriber receipt evidence count differs')
                expected = admitted * expected_subscribers
                delivered = window_total + settle_total
                if delivered > expected: raise ValueError('SchemaMismatch: fanout delivered exceeds expected')
                metrics.update({'fanout.subscriberCount': count_text(expected_subscribers), 'fanout.uniqueDelivered': count_text(delivered), 'fanout.deliveredInWindow': count_text(window_total),
                                'fanout.settleDelivered': count_text(settle_total), 'fanout.duplicateEvents': count_text(duplicates),
                                'fanout.publishOpsPerSec': admitted / source['window']['measuredSeconds'],
                                'fanout.deliveryOpsPerSec': delivery_rate, 'fanout.deliveryRatio': min(ratios) if admitted else None, 'fanout.outOfCohortEvents': count_text(out_of_cohort)})
                if not expected:
                    nulls['/metrics/fanout.deliveryRatio'] = reason('ZERO_DENOMINATOR', 'No measured publisher admission.')
                    issues.append({'code': 'NoPublish', 'message': 'No measured publisher success.', 'sourceFile': ','.join(owner_names)})
                if duplicates: issues.append({'code': 'DuplicateDelivery', 'message': 'Subscriber duplicate events observed.', 'sourceFile': ','.join(receiver_names)})
            for key in ('process.cpuPercent', 'process.rssMb', 'process.allocatedMb', 'gc.gen0', 'gc.gen1', 'gc.gen2', 'load.inflight.max'):
                if len(participants) > 1:
                    metrics[key] = None
                    nulls['/metrics/' + key] = reason('MULTIPLE_OWNERS', 'Independent process observations have no verified simultaneous global observation; see originals.')
            for name, item in originals.items():
                if u64(item['metrics']['load.lateWarmupMessages']): issues.append({'code': 'LateWarmupMessages', 'message': 'Warmup identity observed after measured reset.', 'sourceFile': name})
                if any(item['metrics'][key] for key in CATALOG['errorNamespaces']): issues.append({'code': 'PublicOrApplicationFailure', 'message': 'See original error namespaces and firstErrors.', 'sourceFile': name})
        for name, item in templates.items():
            if item.get('resetSeq') != '1' and any(item.get('metrics', {}).get(key) for key in CATALOG['errorNamespaces']):
                issues.append({'code': 'PreMeasurementFailure', 'message': 'Setup or warmup error evidence preserved.', 'sourceFile': name})
    except (KeyError, TypeError, ValueError, OverflowError) as error:
        issues.append({'code': 'CounterOverflow' if 'Overflow' in str(error) else 'SchemaMismatch' if 'SchemaMismatch' in str(error) else 'CollectionFailure', 'message': str(error), 'sourceFile': ','.join(owner_names)})
    for key, value in metrics.items():
        if value is not None: nulls.pop('/metrics/' + key, None)
        elif '/metrics/' + key not in nulls: nulls['/metrics/' + key] = reason('NOT_APPLICABLE', 'No aggregate observation for this operation.')
    for key, value in histograms.items():
        if value is not None: nulls.pop('/histograms/' + key, None)
    seconds = selected[0]['window']['measuredSeconds'] if len(selected) == 1 else None
    if seconds is None: nulls['/measuredSeconds'] = reason('MULTIPLE_OWNERS' if len(selected) > 1 else 'COLLECTION_FAILED', 'No single primary owner window.')
    fanout_method = 'sum-subscriber-rates' if publish else None
    if fanout_method is None: nulls['/aggregation/fanoutDeliveryRateMethod'] = reason('NOT_APPLICABLE', 'No fanout operation.')
    invalid_codes = {'InvalidSetup', 'SchemaMismatch', 'CounterOverflow', 'StartSkewExceeded', 'ArtifactMismatch', 'LateWarmupMessages', 'ConnectionCountMismatch', 'NoAdmission', 'NoPublish'}
    status = 'unsupported' if any(item['code'] == 'PublicContractMismatch' for item in issues) else 'invalid' if any(item['code'] in invalid_codes for item in issues) else 'failed' if issues else 'valid'
    if status == 'valid' and not one_way and not publish and not u64(metrics.get('messages.completed', '0')):
        status = 'invalid'
        issues.append({'code': 'NoCompletedEcho', 'message': 'No measured validated echo completion.', 'sourceFile': ','.join(owner_names)})
    eligible = status == 'valid' and config.get('diagnostics', 'Off') == 'Off' and not config.get('optionalExperiment')
    if publish:
        threshold = config.get('minDeliveryRatio')
        eligible = eligible and threshold is not None and metrics.get('fanout.deliveryRatio') is not None and metrics['fanout.deliveryRatio'] >= threshold
    result = {'schemaVersion': 3, **{key: config[key] for key in ('runId', 'cellId', 'configHash', 'comparisonKey', 'language', 'scenario')},
              'configFile': 'config.json', 'endpointsFile': 'endpoints.json', 'status': status, 'baselineEligible': eligible,
              'reasons': issues, 'metricOwners': owner_names, 'ownerWindows': {name: originals[name]['window'] for name in owner_names if name in originals},
              'measuredSeconds': seconds, 'aggregation': {'rateMethod': 'sum-owner-rates' if len(owner_names) > 1 else 'single-owner',
              'applicationRateMethod': 'sum-role-rates', 'fanoutDeliveryRateMethod': fanout_method}, 'metrics': metrics, 'histograms': histograms,
              'nullReasons': nulls, 'clients': client_files, 'servers': server_files,
              'processes': [{'sourceFile': name, 'pid': item['provenance']['pid'], 'clock': item['clock'],
                            'resources': {key: value for key, value in item['metrics'].items() if key.startswith(('process.', 'gc.'))},
                            'publicStatusFile': name} for name, item in originals.items()]}
    write_json(cell / 'result.json', result)
    write_json(cell / 'summary.json', {key: result[key] for key in ('schemaVersion', 'runId', 'cellId', 'scenario', 'status', 'baselineEligible', 'reasons', 'metrics', 'metricOwners', 'ownerWindows')})
    (cell / 'summary.txt').write_text(f"{config['scenario']} {config.get('mode')} status={status} baselineEligible={eligible} KOPS={metrics.get('throughput.kops')} p99={metrics.get('latency.p99Ms')} ms\n")
    return result
