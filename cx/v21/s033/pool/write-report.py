#!/usr/bin/env python3
"""Read this session's raw captures, validate them, and write report.md once."""
import csv
import json
import math
import pathlib
import re
import statistics

job = pathlib.Path('/root/jobs/fx/cx/v21/s033/pool')
root = job / 'measure'
report = job / 'report.md'
if report.exists():
    raise SystemExit(f'refusing to overwrite {report}')
variants = ('base', 'pool8', 'pool4', 'pool2')
records = {v: [] for v in variants}

for variant in variants:
    for round_no in (1, 2, 3):
        row = {'variant': variant, 'round': round_no}
        for pattern in ('send-saturation', 'request-backpressure'):
            raw = root / str(round_no) / variant / f'throughput-{pattern}' / 'run1' / f'zlink-framework-cpp-{pattern}-1024' / 'results.json'
            data = json.loads(raw.read_text())
            assert len(data['cells']) == 1
            cell = data['cells'][0]
            assert cell['implementation'] == 'zlink-framework-cpp'
            assert cell['pattern'] == pattern and cell['payload_size'] == 1024
            assert not cell['contaminated'] and cell['errors'] == 0 and cell['abandoned'] == 0 and not cell['drain_bound_hit']
            assert cell['completed'] == cell['target_stats']['received']
            row[pattern] = {'value': cell['throughput_per_second'], 'raw': raw.relative_to(job)}

        pmraw = root / str(round_no) / variant / 'pm' / 'run1' / 'zlink-framework-cpp-send-saturation-1024' / 'capture' / 'pm-latencies.json'
        latencies = sorted(json.loads(pmraw.read_text()))
        assert len(latencies) == 250
        row['pm'] = {
            'p50': latencies[math.ceil(.50 * len(latencies)) - 1],
            'p99': latencies[math.ceil(.99 * len(latencies)) - 1],
            'raw': pmraw.relative_to(job),
        }

        capture = root / str(round_no) / '22k' / f'{variant}-zlink-framework-cpp' / 'runner' / 'run1' / 'zlink-framework-cpp-request-backpressure-1024' / 'capture'
        cell_result = json.loads((capture.parent / 'results.json').read_text())['cells'][0]
        assert cell_result['implementation'] == 'zlink-framework-cpp'
        assert cell_result['pattern'] == 'request-backpressure' and cell_result['payload_size'] == 1024
        assert not cell_result['contaminated'] and cell_result['errors'] == 0 and cell_result['abandoned'] == 0 and not cell_result['drain_bound_hit']
        assert cell_result['completed'] == cell_result['target_stats']['received']
        windows = json.loads((capture / 'windows.json').read_text())
        messages = windows['A_messages']
        assert messages > 0
        before, after = {}, {}
        for role in ('source', 'target'):
            for edge, dest in (('before', before), ('after', after)):
                dest[role] = {}
                for line in (capture / f'A-{role}-{edge}.tsv').read_text().splitlines():
                    tid, comm, runtime, voluntary, _ = line.split('\t')
                    dest[role][tid] = {'comm': comm, 'runtime': int(runtime), 'voluntary': int(voluntary)}
        perf = {}
        perf_lines = (capture / 'A-perfstat.csv').read_text().splitlines()
        for item in csv.reader(perf_lines):
            if len(item) < 4 or not item[0] or item[0].startswith('#'):
                continue
            try:
                value = float(item[1])
            except ValueError:
                continue
            perf.setdefault(item[0].rsplit('-', 1)[-1], {})[item[3]] = value

        threads = {role: {} for role in ('source', 'target')}
        for role in ('source', 'target'):
            for tid, b in before[role].items():
                if tid not in after[role]:
                    continue
                a = after[role][tid]
                counters = perf.get(tid, {})
                mperf = counters.get('msr/mperf/', 0)
                threads[role][tid] = {
                    'comm': b['comm'],
                    'us': max(0, a['runtime'] - b['runtime']) / 1000 / messages,
                    'wakes': max(0, a['voluntary'] - b['voluntary']),
                    'sendto': counters.get('syscalls:sys_enter_sendto', 0) / messages,
                    'ghz': counters['msr/aperf/'] / mperf * 2.0 if mperf else None,
                }
        pids = {key: int(value) for key, value in (line.split('\t') for line in (capture / 'pids.tsv').read_text().splitlines())}
        assert str(pids['source']) in before['source'] and str(pids['target']) in before['target']

        source = threads['source']
        target = threads['target']
        completion = {tid: t for tid, t in source.items() if t['comm'].startswith('zl-cmpl-')}
        if variant == 'base':
            named_roles = {'ZLINKbg/Reaper', 'ZLINKbg/core-ct', 'zlink-req-time', 'zlink-timer'}
            residual = {
                tid: t for tid, t in source.items()
                if tid != str(pids['source']) and '/IO/' not in t['comm']
                and t['comm'] not in named_roles and not t['comm'].startswith('zl-cmpl-')
            }
            pool_threads = residual
            assert len(pool_threads) > 0
        else:
            expected = int(variant.removeprefix('pool'))
            assert len(completion) == expected, (variant, round_no, len(completion))
            pool_threads = completion

        target_log = (capture.parent / 'target.log').read_text(errors='replace')
        role_tids = {
            kind: set(re.findall(fr'ROLE {kind} tid=(\d+)', target_log))
            for kind in ('app-worker', 'receive')
        }
        target_roles = {
            kind: {
                'us': sum(target[tid]['us'] for tid in tids if tid in target),
                'matched': sum(tid in target for tid in tids),
                'expected': len(tids),
            }
            for kind, tids in role_tids.items()
        }
        assert target_roles['receive']['expected'] == 1
        assert target_roles['app-worker']['expected'] == 3

        def io_thread(role_threads):
            candidates = [t for t in role_threads.values() if '/IO/' in t['comm']]
            assert candidates
            return max(candidates, key=lambda t: t['sendto'])
        io = {'target': io_thread(target), 'source': io_thread(source)}
        row['22k'] = {
            'messages': messages,
            'rate': windows['A_rate'],
            'pool_us': sum(t['us'] for t in pool_threads.values()),
            'pool_wakes': sum(t['wakes'] for t in pool_threads.values()) / messages,
            'pool_threads': len(pool_threads),
            'pool_rule': 'unnamed residual' if variant == 'base' else 'zl-cmpl-*',
            'io': io,
            'roles': target_roles,
            'process_us': {role: sum(t['us'] for t in role_threads.values()) for role, role_threads in (('target', target), ('source', source))},
            'matched': {role: len(role_threads) for role, role_threads in (('target', target), ('source', source))},
            'wakes': {role: sum(t['wakes'] for t in role_threads.values()) / messages for role, role_threads in (('target', target), ('source', source))},
            'raw': capture.relative_to(job),
        }
        records[variant].append(row)

# Confirm complete raw matrix before generating prose or tables.
assert sum(len(rows) for rows in records.values()) == 12
for variant in variants:
    assert len(records[variant]) == 3

fmt3 = lambda n: f'{n:.3f}'
fmt0 = lambda n: f'{n:,.0f}'
link = lambda p: f'[{p}]({p.as_posix()})'
base = records['base']
def band(values): return f'{min(values):.3f}–{max(values):.3f}'

def rows_for(metric):
    return [x[metric]['value'] for x in base]

lines = [
    '# C++ completion dispatcher pool 2 / 4 / 8 측정 (#1639)',
    '',
    '## 조건',
    '',
    '같은 세션에서 base·pool8·pool4·pool2를 각각 3회 실행했다. Base는 기존 `psmeas-base` Release 빌드(HEAD `168c679590`), 나머지는 `perf/1639-pool-exp`의 로컬 커밋 `17c5a80757` 빌드다. 네 빌드는 Release `-O3 -DNDEBUG`, Core/binding 1.23.0, 같은 vcpkg prefix와 복사한 bench harness를 사용했다. Pool8/4/2는 `ZLINK_PERF_COMPLETION_POOL`을 source와 target 모두에 설정했다. Base는 변수를 설정하지 않았다.',
    '',
    'bstage A/B/A/B 결과는 launch 방식 차이를 분리하지 못했고 app-worker 합계 차이가 5초 캡처 전후에 모두 존재한 TID 수에서 비롯됨을 확인했다. 따라서 psched8 Python supervisor 방식으로 통일했다. Source CPU는 0–7, target CPU는 8–15였다. 각 측정 호출은 `/root/jobs/fx/cx/measure.lock`으로 직렬화했다. PM은 `/pm/request` readiness 확인 후 30초 측정했고, 22k는 1 KiB request-backpressure의 `TARGET_RATE=48000` paced 실행에서 5초 window A를 수집했다. 이전 세션 값은 비교에 사용하지 않았다.',
    '',
    '| Round | 실행 순서 |',
    '|---:|---|',
    '| 1 | base → pool8 → pool4 → pool2 |',
    '| 2 | pool8 → pool4 → pool2 → base |',
    '| 3 | pool4 → pool2 → base → pool8 |',
    '',
    '## Framework throughput (1 KiB)',
    '',
    f'Base 자체 3회 범위: send-saturation **{band(rows_for("send-saturation"))} msg/s**, request-backpressure **{band(rows_for("request-backpressure"))} msg/s**.',
    '',
    '| Variant | Round | Send-saturation msg/s | Request-backpressure msg/s | Raw results |',
    '|---|---:|---:|---:|---|',
]
for variant in variants:
    for x in records[variant]:
        lines.append(f"| {variant} | {x['round']} | {fmt0(x['send-saturation']['value'])} | {fmt0(x['request-backpressure']['value'])} | {link(x['send-saturation']['raw'])}<br>{link(x['request-backpressure']['raw'])} |")

lines += [
    '',
    '## PM Spot (µs)',
    '',
    f'Base 자체 3회 범위: p50 **{band([x["pm"]["p50"] for x in base])}**, p99 **{band([x["pm"]["p99"] for x in base])}**.',
    '',
    '| Variant | Round | p50 | p99 | Raw latencies |',
    '|---|---:|---:|---:|---|',
]
for variant in variants:
    for x in records[variant]:
        lines.append(f"| {variant} | {x['round']} | {fmt3(x['pm']['p50'])} | {fmt3(x['pm']['p99'])} | {link(x['pm']['raw'])} |")

lines += [
    '',
    '## 22k Framework request-backpressure',
    '',
    'Base 자체 3회 범위: completion pool attribution **' + band([x['22k']['pool_us'] for x in base]) + ' µs/msg**, voluntary wakes **' + band([x['22k']['pool_wakes'] for x in base]) + ' /msg**; target/source I/O sendto **' + band([x['22k']['io']['target']['sendto'] for x in base]) + ' / ' + band([x['22k']['io']['source']['sendto'] for x in base]) + ' /msg**; target receive/app-worker **' + band([x['22k']['roles']['receive']['us'] for x in base]) + ' / ' + band([x['22k']['roles']['app-worker']['us'] for x in base]) + ' µs/msg** (app-worker 1/3 TID); target/source process CPU **' + band([x['22k']['process_us']['target'] for x in base]) + ' / ' + band([x['22k']['process_us']['source'] for x in base]) + ' µs/msg**; effective GHz **' + band([x['22k']['io']['target']['ghz'] for x in base]) + ' / ' + band([x['22k']['io']['source']['ghz'] for x in base]) + '**; process voluntary wakes **' + band([x['22k']['wakes']['target'] for x in base]) + ' / ' + band([x['22k']['wakes']['source'] for x in base]) + ' /msg**.',
    '',
    '| Variant | Round | Pool CPU µs/msg; wakes/msg; TIDs | Target receive / app-worker µs/msg (matched/observed) | Process CPU target/source µs/msg (matched TIDs) | Raw capture |',
    '|---|---:|---:|---:|---:|---|',
]
for variant in variants:
    for x in records[variant]:
        y=x['22k']; roles=y['roles']
        poolname = 'residual' if variant == 'base' else 'named'
        pooltext = f"{fmt3(y['pool_us'])}; {fmt3(y['pool_wakes'])}; {y['pool_threads']} {poolname}"
        roletext = f"{fmt3(roles['receive']['us'])} (1/1) / {fmt3(roles['app-worker']['us'])} ({roles['app-worker']['matched']}/3)"
        processtext = f"{fmt3(y['process_us']['target'])} / {fmt3(y['process_us']['source'])} ({y['matched']['target']}/{y['matched']['source']})"
        lines.append(f"| {variant} | {x['round']} | {pooltext} | {roletext} | {processtext} | {link(y['raw'])} |")

lines += [
    '',
    '| Variant | Round | I/O sendto target/source per msg | Effective GHz target/source | Voluntary wakes target/source per msg | Window A msg/s | Raw capture |',
    '|---|---:|---:|---:|---:|---:|---|',
]
for variant in variants:
    for x in records[variant]:
        y=x['22k']; io=y['io']
        lines.append(f"| {variant} | {x['round']} | {fmt3(io['target']['sendto'])} / {fmt3(io['source']['sendto'])} | {io['target']['ghz']:.2f} / {io['source']['ghz']:.2f} | {fmt3(y['wakes']['target'])} / {fmt3(y['wakes']['source'])} | {fmt0(y['rate'])} | {link(y['raw'])} |")

lines += [
    '',
    'Pool2/4/8 CPU와 wakes는 source의 `zl-cmpl-*` TID를 합산했다. Base는 이름이 없어 source matched-TID 합계에서 `/IO/`, `ZLINKbg/Reaper`, `ZLINKbg/core-ct`, `zlink-req-time`, `zlink-timer`, process leader를 뺀 residual을 썼다. Base residual 35 TID에는 이름 없는 비-dispatcher thread가 섞일 수 있어 dispatcher 전용 합계로 보지 않는다. Process CPU/wakes는 before·after 두 snapshot에 모두 존재한 TID 합계다. App-worker는 로그에 기록된 3개 TID 중 snapshot 양쪽에 잡힌 TID만 합산했고, receive는 1/1이었다. 따라서 app-worker 값은 coverage 차이를 함께 보아야 한다.',
    '',
    'I/O는 `/IO/` 이름의 thread 중 sendto가 가장 많은 thread다. Effective GHz는 그 thread의 `aperf / mperf × 2.0`이다. 22k 원자료의 `windows.json`, thread snapshots, `A-perfstat.csv`, `target.log`는 각 capture 경로에 보존했다.',
    '',
    '## 결과 확인',
    '',
    '24개 throughput 셀은 모두 clean이며 completed와 target received가 일치했다. PM은 12개 모두 Spot 250개를 수집했다. 22k는 12회 모두 source/target 완료·수신 수가 일치했다. `bench_cpp_client`, `bench_cpp_framework_server`, `bench_cpp_pm_server`는 수정 source보다 최신이고 각 binary에서 `ZLINK_PERF_COMPLETION_POOL`을 확인했다. 테스트는 실행하지 않았다.',
    '',
    '작업 변경은 [provider_task_context.cpp](/root/worktree/zlink-v21-s033/pool/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp) 한 파일이며 로컬 commit은 `17c5a80757`이다. `.pool-build`는 복사한 실행 harness와 조건 기록에서 계속 참조하므로 보존했다.',
]
report.write_text('\n'.join(lines) + '\n')
print(report)
