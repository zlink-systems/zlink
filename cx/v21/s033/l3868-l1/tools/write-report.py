#!/usr/bin/env python3
"""Render only this session's checked raw results; preserve inconclusive cells."""
import datetime
import json
from pathlib import Path
import subprocess

JOB = Path('/root/jobs/fx/cx/v21/s033/l3868-l1')
REPO = Path('/root/worktree/zlink-v21-s033/l3868')


def link(value, path):
    return f'[{value}]({path})'


def median_link(variant):
    record = min(variant['runs'], key=lambda r: abs(r['msg_s'] - variant['median']))
    return link(f'{variant["median"]:,.0f}', record['path'])


def main():
    summary_path = JOB / 'raw/summary.json'
    cells = json.loads(summary_path.read_text())['cells'] if summary_path.exists() else []
    head = subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()
    lines = ['# #4350 C++ L0 재확인 · #4351 L1 permit', '',
             f'실험 브랜치 `exp/3868-cpp-ladder`, 로컬 commit `{head}`. '
             '[변경 파일](raw/commit.txt) · [patch](raw/source-change.patch). Push·PR은 하지 않았다.', '',
             'L0·L1은 같은 Release 바이너리에서 `ZLINK_CPP_LADDER_LEVEL=0/1`로 선택한다. '
             'Source send·request·reply completion은 binding과 공통 `submit_raw`를 호출한다. '
             'Target은 같은 blocking recv와 새 envelope·새 protobuf body의 2-part reply를 유지한다. '
             'L1만 실제 application job queue permit을 수신 전에 확보하고 handler 첫 instruction 직전에 반환한다. '
             '[전체 호출 대조](call-shape.md) · [매 실행 전 대조·출처](progress.md).', '',
             '측정 조건과 출처는 [build seal](raw/build-provenance.json), '
             '[lock 시작](raw/lock-windows/L0-L1-before.txt), '
             '[session 시작](raw/session-start.json)·[종료](raw/session-end.json), '
             '[시간 한도](raw/task-time.json)에 보존했다. '
             'binding·L0·L1은 한 exclusive lock 세션에서 같은 셀·횟수로 측정했다.', '',
             '아래 중앙값 단위는 msg/s다. G5 spread는 `max(|run−median|)/median`이다. '
             '공식 aggregator와 별도 재계산을 대조했다. G5가 실패한 셀은 판정 불가다.', '',
             '| L0 셀 | 횟수 | binding 중앙값 | L0 중앙값 | L0÷binding | G5 binding / L0 | 판정 |',
             '|---|---:|---:|---:|---:|---:|---|']
    by_key = {(c['pattern'], c['size']): c for c in cells}
    for pattern in ('send-saturation', 'request-serial', 'request-backpressure'):
        for size in (1024, 4096):
            c = by_key.get((pattern, size))
            label = f'{pattern} {size // 1024}K'
            if not c:
                lines.append(f'| {label} | — | 미완료 | 미완료 | — | — | 판정 불가 |')
                continue
            v = c['variants']; raw = f'raw/cells/{pattern}-{size}.json'
            lines.append(f'| {label} | {link(len(v["L0"]["runs"]), raw)} | {median_link(v["binding"])} | '
                         f'{median_link(v["L0"])} | {link(format(c["L0_ratio"], ".4f"), raw)} | '
                         f'{link("{:.2f}% / {:.2f}%".format(100*v["binding"]["spread"], 100*v["L0"]["spread"]), raw)} | '
                         f'{c["L0_verdict"]} |')
    lines += ['', '| L1 셀 | L1 중앙값 | L1÷binding | L1 step loss | G5 L1 | 판정 |',
              '|---|---:|---:|---:|---:|---|']
    for pattern in ('send-saturation', 'request-serial', 'request-backpressure'):
        for size in (1024, 4096):
            c = by_key.get((pattern, size)); label = f'{pattern} {size // 1024}K'
            if not c:
                lines.append(f'| {label} | 미완료 | — | — | — | 판정 불가 |'); continue
            v = c['variants']; raw = f'raw/cells/{pattern}-{size}.json'
            lines.append(f'| {label} | {median_link(v["L1"])} | '
                         f'{link(format(c["L1_ratio"], ".4f"), raw)} | '
                         f'{link(format(100*c["step_loss"], ".2f") + "%", raw)} | '
                         f'{link(format(100*v["L1"]["spread"], ".2f") + "%", raw)} | {c["L1_verdict"]} |')
    lines += ['', 'L0 기준은 `L0÷binding ≥0.95`이며 L1 step loss는 '
              '`1−(L1÷binding)/(L0÷binding) ≤7%`다. 분모와 이전 단계 값은 모두 이번 세션의 값이다. '
              '[계산 원본](raw/summary.json).', '']
    outliers = [(c, variant, r) for c in cells for variant, v in c['variants'].items() for r in v['outliers']]
    if outliers:
        lines += ['| G5 outlier | variant | 시작 UTC | msg/s |', '|---|---|---|---:|']
        for c, variant, r in outliers:
            lines.append(f'| {c["pattern"]} {c["size"] // 1024}K {Path(r["path"]).parts[-3]} | '
                         f'{variant} | {link(r["start_utc"], r["path"].replace("results.json", "run-start.txt"))} | '
                         f'{link(format(r["msg_s"], ",.0f"), r["path"])} |')
        lines += ['']
    lines += ['| 복원 위치 | 동작 |', '|---|---|',
              '| [raw_mesh_node_owner.cpp:535](/root/worktree/zlink-v21-s033/l3868/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:535) | 실제 `wait_for_supply_blocking(remote)` 후 같은 recv; 실패는 permit RAII 반환 |',
              '| [ladder_wire.hpp:303](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp:303) | 실제 Balanced capacity resolver로 queue 생성 |',
              '| [ladder_wire.hpp:322](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_wire.hpp:322) | 실제 `release_for_handler_entry` 다음 고정 handler 호출 |',
              '| [ladder_driver.inc:115](/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/ladder/ladder_driver.inc:115) | L0·L1 모두 binding 공통 source submit |', '',
              '소유 계층: permit은 Framework `application_job_queue_t`, 물리 admission·receive·completion은 Core/binding이다.', '',
              '스펙: [Application job queue §3](/root/worktree/zlink-v21-s033/l3868/framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:117)의 수신 전 확보·handler 직전 반환을 사용했다.', '',
              '교차언어 대조: Java `acquireBlocking(REMOTE)`·`handlerStarted`, .NET L1 `b6f58168a6`의 `AcquireAsync(Remote)`·`ReleaseForHandlerStartAsync`와 같은 경계다.', '',
              '변경 분류: C, 사용자가 승인한 실험 전용 permit 복원이다. 제품 A/B 수정을 주장하지 않는다.', '',
              '규칙 수: 실험 경로의 permit 판정 위치 0→1(실제 queue), 물리 admission/completion 1→1(Core/binding). '
              '기존 queue의 상태를 재사용했고 별도 counter·queue·lock은 추가하지 않았다.', '',
              '호출 경로 점검: L1 source가 별도 encoder를 사용하던 비활성 경로를 공통 submit으로 맞췄다. '
              '범위 밖 제품 수정으로 넘긴 항목은 없다.', '',
              '검증: [built unit tests 1회](raw/bench-tests.txt), [wire 양성·음성 검사](raw/wire-check.txt), '
              '[normal runtime 동일성](raw/normal-runtime.txt), [수신·응답·source 대조](raw/parity.txt), '
              '[최종 raw 재검사](raw/final-audit.json).', '',
              '정리: [build-l3868 제거·작업 프로세스 확인](raw/cleanup.json). 원시 결과는 모두 보존했다.']
    if any(c['step_loss'] > .07 for c in cells):
        lines += ['', 'L1 step loss 초과 셀의 [paired perf·cost 위치·binding-cost 제안](raw/cost-findings.md)을 보존했다. 제안은 구현하지 않았다.']
    (JOB / 'report.md').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
