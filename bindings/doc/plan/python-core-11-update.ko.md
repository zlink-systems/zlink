# Python binding Core 0.9.0 최신화 실행 계획

> 이 문서는 Python binding의 raw Core 0.9.0 구현, package, sample, perf와 검증 evidence를 관리하는 실행
> 계획이다. Python public contract 자체는 `bindings/doc/spec/python/`이 소유한다.

## 1. 현재 판정

현재 checkout의 Core는 `11.2.0`이다. Python binding은 이 Core의 raw C API만 연결하도록 정리했고,
Linux x86_64에서 source test, clean wheel consumer, raw sample process와 perf smoke까지 통과했다. 다만
다음 조건이 남아 있으므로 전체 완료 판정은 `PARTIAL / NOT CLEAN`이다.

- 현재 `core/build`에서 만든 `11.2.0` candidate와 저장소에 있는 공통 `V11-R2`·`V11-M3-CORE-PKG`
  evidence가 같은 candidate identity인지 확인되지 않았다. 이전 `11.1.0` evidence를 `11.2.0`의 승인으로
  재사용하지 않는다.
- CPython 3.9.25와 host CPython 3.12.3 clean consumer를 같은 candidate 절차로 다시 실행했다. Pyright
  target은 Python 3.9로 유지한다. 최신 package snapshot과 hash는 progress log의 self-review evidence에 기록한다.
- release 지원 target은 Linux x86_64로 한정했고, 다른 target은 setup·loader에서 fail-fast하도록 정리했다.
- 독립 frontier reviewer는 초기 candidate를 `NOT CLEAN`으로 판정했다. Python source의 High·Medium
  finding은 수정했다. 최종 재검토에서 공통 spec에 Core 0.9.0에 없는 raw socket channel metadata 선언과
  binding rule이 남은 사실이 추가로 확인되어 두 공통 spec과 contract guard를 수정했다. 이 수정 뒤 최종
  source manifest와 fresh package evidence를 다시 읽은 `CLEAN` 판정은 아직 없다.

이 조건들은 source test나 local package 결과를 실패로 바꾸는 것이 아니라, 완료 gate와 local implementation
evidence를 구분하기 위한 것이다.

## 2. 입력과 책임 경계

이번 작업의 local candidate는 다음 입력을 사용한다.

| 항목 | 현재 값 또는 위치 |
|------|------------------|
| Core version | `11.2.0` |
| Core runtime | `core/build/lib/libzlink.so.0.2.0` |
| Core SONAME | `libzlink.so.0` |
| Python package | `zlink==11.2.0` |
| Python source manifest | progress log self-review evidence에 기록한 각 interpreter의 manifest |
| package evidence | 각 self-review output root의 `python/candidate-input.env` |
| wheel | 각 self-review output root의 `python/wheels/` 아래 산출물 |

Core candidate의 header, spec, source, runtime, exported symbol inventory와 layout은
[`create-manifest.sh`](../../../scripts/local-package/bindings-candidate/create-manifest.sh)가 봉인한다.
Python source manifest는 Python source, test, sample, perf, package script와 Python spec/guide를 기록하고
Core manifest SHA-256을 direct input으로 포함한다. Candidate package script는 두 manifest의 revision과
hash가 현재 checkout과 일치하지 않으면 중단한다.

DDD 기준의 경계는 다음과 같다.

- Core raw bounded context는 `Context`, `Message`, `Received`, `RoutingId`, raw socket, monitor, poller,
  timer와 Core error 의미를 소유한다.
- Python adapter는 native handle, `ctypes` layout, callback trampoline, reference lifetime와 Python
  exception mapping을 내부에 둔다. 이 결정은 public type으로 노출하지 않는다.
- Framework 기능의 actor, spot, dispatch와 bound stream session은 Framework 경계가 소유한다. Python Core
  binding은 이 lifecycle을 재정의하지 않는다.
- `Context`, `Message`/`Received`, socket과 package adapter가 각각 handle·buffer·callback·candidate
  provenance의 aggregate owner다. 자세한 event와 invariant는
  [`POSD·DDD 검토 log`](log/python/2026-08-03-posd-ddd-review.ko.md)에 기록한다.

## 3. 구현 범위와 완료 사실

### PY-01 — Raw FFI와 Core 0.9.0 projection — PASS

- `ffi.py`와 `_zlink_native.c`는 Core 0.9.0 raw symbol과 layout만 선언한다. 공식 perf runner는 public
  Python contract를 사용하므로 별도 private perf native extension을 package에 포함하지 않는다.
- Core header에 없는 이전 기능의 FFI, callback, include와 compiled entrypoint를 제거했다.
- `ctx_set_data`/`ctx_get_data`가 필요한 `uint64` option은 Core 함수군의 실제 ABI에 맞춰 연결했다.
- `Message`, `Received`, routing id, raw socket, monitor, poller와 timer의 production path를 유지했다.
- source test와 raw sample에서 native error, no-data, move/close ownership을 확인했다.

### PY-02 — Public API와 bounded context 정리 — PASS

- package root와 contracts에서 Framework 전용 public surface와 이전 compatibility alias를 제거했다.
- raw socket에 Framework lifecycle을 섞던 branch와 helper, 관련 fixture, sample과 perf scenario를 제거했다.
- dynamic `__getattr__`로 public contract를 숨기지 않고 static export를 사용한다.
- `single_part_or_throw()`는 현재 구현과 contract test가 사용하는 이름을 유지한다. 이름을
  `single_part()`로 바꾸는 draft는 별도 review와 contract 변경 없이는 적용하지 않는다.

### PY-03 — Error, no-data와 ownership — PASS

- no-data는 함수군별 계약에 따라 `False` 또는 `None`으로 반환하고 native failure를 숨기지 않는다.
- submit, request, receive, bind, connect, config와 close error가 result와 native errno를 보존한다.
- message send 후 move, caller-provided receive storage, callback reference와 idempotent close를 test로
  고정했다. native close가 `EBUSY`를 반환하면 socket, monitor, timer, context의 handle과 callback owner를
  유지하고 성공 뒤에만 정리한다. invalid payload는 backpressure인 `False`로 바꾸지 않는다.

### PY-04 — POSD·DDD와 hot-path cost — PASS (self-review)

- message allocation, receive owner, callback dispatcher, GIL 해제와 snapshot copy를
  `tests/hot-path-cost-inventory.json`에 owner와 guard test와 함께 기록했다.
- blocking native wait 중 Python object에 접근하지 않고, callback 경계에서만 Python 실행 상태를 복원한다.
- 기존 raw module을 유지하면서 Framework branch·export·FFI·fixture만 제거하는 대안을 선택했다. 새 facade를
  추가하는 재작성안은 public surface와 ownership 변경을 늘리므로 선택하지 않았다.
- optimization guard, source review와 runtime sample을 통과했다. 독립 reviewer가 아니므로 이 항목은 전체
  `CLEAN` 판정이 아니다.

### PY-05 — Sample과 perf — PASS (Linux x86_64)

Canonical sample은 다음 7개다.

- `request_reply_callback_sample.py`
- `pair_recv_sample.py`
- `dealer_router_recv_sample.py`
- `pubsub_recv_sample.py`
- `stream_recv_sample.py`
- `stream_packet_callback_sample.py`
- `monitor_recv_sample.py`

`samples/run_samples.py --installed`는 clean wheel consumer에서 sample directory만 `PYTHONPATH`에 두고,
repository `src`를 import하지 않는다. Single·multi perf runner는 `ZLINK_LIBRARY_PATH`를 명시적으로 받고
실행 runtime path와 SHA-256을 출력한다. `--smoke` 결과는 성능 report가 아니라 process lifecycle과 필수
`RESULT` row 확인으로만 사용한다.

### PY-06 — Package와 clean consumer — PASS (Linux x86_64)

`setup.py`는 `ZLINK_CORE_PREFIX`를 요구하고 repository `core/build`를 implicit build input으로 사용하지
않는다. Candidate package script는 manifest에서 만든 candidate prefix로 다음을 모두 확인한다.

1. Core manifest의 revision, version, header/spec/source hash, runtime hash, SONAME, symbol inventory,
   layout과 freshness를 다시 확인한다.
2. candidate prefix로 extension과 wheel을 build한다.
3. wheel에 `py.typed`와 현재 Linux x86_64 runtime 하나만 있는지 확인하고, 이전 SONAME·`libzlink_c`·다른
   platform payload·source path가 있으면 실패한다.
4. 새 virtual environment에 `--no-deps`로 설치하고 repository 밖 cwd에서 `PYTHONPATH`, `LD_LIBRARY_PATH`,
   `ZLINK_LIBRARY_PATH`를 제거한다.
5. `zlink.version()`과 실제 Pair message roundtrip을 실행하고 `/proc/self/maps`에서 venv의 wheel payload가
   load된 것을 확인한다.
6. 같은 clean environment에서 `run_samples.py --installed`를 실행해 7개 process sample을 확인한다.
7. source manifest를 package build 뒤 다시 생성해 source drift를 거부한다.

### PY-07 — 정식 문서 — PASS (현재 구현 기준)

한국어·영문 Python spec과 한국어 guide는 raw Core 0.9.0 public surface, ownership, no-data, error, Python 3.9
type policy와 현재 `11.2.0` candidate를 설명한다. 구현에 없는 Framework 기능과 이전 Core runtime을 지원
목록으로 제공하지 않는다. Python callback 표면과 private FFI callback의 경계를 명시하고, `single_part()`
draft는 승인 전이므로 정식 문서는 현재 accessor 이름을 유지한다.

## 4. Platform 범위와 검증

이번 Core 0.9.0 Python package는 Linux x86_64만 release target으로 지원한다. 다른 target의 native
payload를 package에 남겨 두거나 loader에서 자동 선택하지 않으며, 별도 candidate와 clean consumer
evidence가 생기기 전에는 지원 범위로 표시하지 않는다.

| Target | 상태 | 근거 |
|--------|------|------|
| Linux x86_64 | `PASS` | candidate wheel, clean consumer, 7 samples, Pair roundtrip, load map |
| Linux aarch64 | `OUT OF SCOPE` | setup·loader·release fetch가 Linux x86_64 외 target을 지원하지 않음 |
| macOS x86_64/aarch64 | `OUT OF SCOPE` | setup·loader가 non-Linux target을 거부함 |
| Windows x86/x86_64/aarch64 | `OUT OF SCOPE` | setup·loader가 non-Linux target을 거부함 |

Linux x86_64 결과를 다른 target의 완료 근거로 승격하지 않는다. 다른 target을 지원 목록에 넣으려면
같은 Core candidate identity로 payload를 build하고 package·native consumer·loader evidence를 추가한
뒤 별도 범위 변경으로 검토한다.

## 5. 검증 명령과 ledger

실행 시점에는 다음 명령의 종료 코드와 산출물 hash를
[`python progress log`](log/python/2026-08-03-core11-progress.ko.md)에 기록한다.

```bash
# Core candidate manifest
scripts/local-package/bindings-candidate/create-manifest.sh \
  .artifacts/wsl/bindings-candidate/core-11.2.0.env

# Candidate wheel, clean consumer와 clean-wheel sample process
scripts/local-package/bindings-candidate/build-wsl.sh \
  --language python \
  --manifest .artifacts/wsl/bindings-candidate/core-11.2.0.env \
  --package-version 11.2.0 \
  --python-executable python3.12 \
  --output "$PWD/.artifacts/wsl/bindings-candidate/python312"

# CPython 3.9 Docker에서는 --python-executable python과 python39 output root를 사용한다.

# Source tests
ZLINK_LIBRARY_PATH="$PWD/core/build/lib/libzlink.so.0.2.0" \
  PYTHONPATH=bindings/python/src PYTHONDONTWRITEBYTECODE=1 \
  pytest -q bindings/python/tests

# Single perf smoke
ZLINK_LIBRARY_PATH="$PWD/core/build/lib/libzlink.so.0.2.0" \
  LD_LIBRARY_PATH="$PWD/bindings/python/src/zlink/native/linux-x86_64" \
  PYTHONPATH=bindings/python/src \
  bindings/python/perf/run_benchmarks.sh --smoke --pattern PAIR \
  --duration 1 --msg-sizes 64 --transports inproc --runs 1

# Multi perf smoke
ZLINK_LIBRARY_PATH="$PWD/core/build/lib/libzlink.so.0.2.0" \
  LD_LIBRARY_PATH="$PWD/bindings/python/src/zlink/native/linux-x86_64" \
  PYTHONPATH=bindings/python/src \
  bindings/python/perf/run_benchmarks_multi.sh --smoke \
  --pattern DEALER_ROUTER --duration 1 --msg-sizes 64 --transports tcp \
  --runs 1 --clients 1
```

| Gate | 현재 상태 | evidence 또는 남은 조건 |
|------|----------|------------------------|
| Local Core candidate 입력 | `PASS` | progress log self-review evidence의 Core manifest와 각 candidate-input의 동일 revision/hash |
| 공통 승인 candidate와의 identity | `BLOCKED` | 현재 11.2.0에 대응하는 독립 V11-R2·V11-M3 evidence 확인 필요 |
| Python source manifest | `PASS` | `python-source-manifest-11.2.0.json`, aggregate와 direct input hash |
| 승인 prefix native build | `PASS` | candidate build가 `ZLINK_CORE_PREFIX`로 wheel build |
| Raw FFI·symbol·layout | `PASS` | source tests, candidate manifest symbol/layout hash |
| Public API와 Framework surface 부재 | `PASS` | export/guard test와 source scan |
| Contract·unit test | `PASS` | `pytest`: 66 passed; poller close retry, callback ownership, invalid payload, `try_copy_to`와 surface guard 포함 |
| `single_part` naming draft | `PASS` | 현재 public contract인 `single_part_or_throw()`를 유지; 별도 draft는 승인 전 설계 후보로 분리 |
| Python 3.9 runtime와 최고 version | `PASS` | CPython 3.9 Docker와 host CPython 3.12 clean consumer |
| `pyright`·`py.typed` | `PASS` | public contracts 대상 pyright 0 errors, wheel file check |
| Hot-path inventory·optimization guard | `PASS` | `unclassified=0`, guard test PASS |
| Raw sample process | `PASS` | clean wheel `--installed`, 7/7 |
| Perf smoke | `PASS` | single/multi RESULT와 runtime hash 출력 |
| Wheel provenance·clean consumer | `PASS` | wheel SHA, payload SHA, SONAME/symbol, roundtrip, load map |
| Linux x86_64 platform | `PASS` | candidate wheel과 clean consumer |
| Support target decision | `PASS` | release package 범위를 Linux x86_64로 한정하고 unsupported target은 fail-fast |
| POSD·DDD Codex self-review | `PASS` | 검토 log와 cost inventory; independent review 아님 |
| 독립 frontier review | `RECHECK REQUIRED` | 초기 reviewer의 High·Medium finding은 수정했으며 같은 최종 manifest와 fresh evidence로 재검토 필요 |
| 정식 spec·guide | `PASS` | 현재 구현과 일치하도록 갱신 |

## 6. 완료 조건과 재개 순서

Python 작업을 전체 완료로 올리려면 local implementation evidence에 더해 공통 candidate identity, Python 3.9
clean consumer, 지원 platform 범위와 독립 review를 결정해야 한다. 현재는 다음 순서로 재개한다.

1. Core source와 package evidence가 같은 `11.2.0` candidate인지 공통 gate 담당자가 확인한다. 이전 `11.1.0`
   evidence는 재사용하지 않는다.
2. Python 3.9 clean wheel import, raw roundtrip, sample과 public type check를 실행했다. host의 최고
   version인 CPython 3.12 clean consumer도 실행했다.
3. release 지원 target은 Linux x86_64로 한정한다. 추가 target을 지원하려면 별도 candidate payload와
   consumer evidence를 만든 뒤 범위 변경으로 검토한다.
4. 독립 reviewer가 최종 Python source manifest, 전체 diff, POSD·DDD cost inventory와 fresh test/package
   evidence를 read-only로 재확인한다. 미해결 `Critical`·`High`·`Medium` finding이 없을 때만 `CLEAN`으로
   갱신한다.
5. 위 조건을 충족한 뒤에만 final ledger와 release-facing 문서의 완료 상태를 갱신한다.

## 7. 2026-08-04 self-review refresh

Codex가 현재 Core 0.9.0.2.0 candidate의 Core diff, Python public contract, owner-layer lifecycle과
POSD·DDD 경계를 직접 다시 검토했다. 이 과정에서 `Message.try_copy_to()`의 contract 반환값 불일치를
찾아 owner layer와 contract test를 수정했다. 이후 Python source test는 `65 passed`, `pyright`는
`0 errors, 0 warnings, 0 informations`, Core weighted-selection integration은 `17/17`, typed option
unit은 `2/2`로 통과했다.

추가 재검토에서 `NativePoller.close()`가 native destroy 실패 전에 `_handle`을 지우는 lifecycle 오류를
확인했다. 다른 native owner와 같은 성공 후 상태 전환으로 수정하고 실패 뒤 재시도 contract test를
추가했다. 이 수정 뒤 Python source test는 `66 passed`로 갱신됐다.

현재 local package의 Core runtime SHA는
`ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138`이다. CPython 3.9.25와
3.12.3 package/clean consumer를 각각 통과했고, 두 installed sample은 `7/7`이다. single·multi perf
smoke도 현재 runtime에서 완료됐다. package snapshot, manifest와 wheel hash는
[`python progress log`](log/python/2026-08-03-core11-progress.ko.md)의 최신 self-review package
refresh 절에 있다.

단, 이것은 구현자에 의한 self-review다. 기존 V11-R2 evidence는 현재 candidate를 승인하지 않으므로
공통 담당자의 fresh V11-R2·V11-M3-CORE-PKG evidence와 독립 frontier review가 완료되기 전에는 전체
상태를 `CLEAN` 또는 완료로 바꾸지 않는다.
