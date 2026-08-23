# C++ 첫 paired smoke 시도 기록 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> §7.1(Pattern별 smoke), §7.2(반복 횟수), §9.1.1(Single suite 표)
>
> 목표: C++ single suite의 첫 `pattern + transport` 조합(§9.1.1 표 첫 행 =
> `PAIR` / `tcp`)에 대해 C reference smoke를 먼저 통과시킨 뒤 C++ binding smoke를
> 같은 조건으로 이어서 실행한다.
>
> 결과: **C reference smoke가 `status: complete`에 도달하지 못했다.** 계획서
> §7.3("C pattern 측정이 끝나면 다른 pattern을 실행하지 않고 바로 같은 binding
> pattern을 측정한다")과 §7.1("C와 binding의 pattern별 smoke가 모두
> `status: complete`여야 본 측정을 시작한다")에 따라 **C++ smoke는 실행하지
> 않았다.**

## 1. 선택한 대상과 이유

- 언어: C++ (진행 순서표 1번, `doc/perf/perf/bindings-0.12.0/progress.ko.md` §1)
- suite: single (계획서 §7.1은 pattern 하나·transport 하나로 시작하도록 요구)
- pattern: `PAIR` — C single runner의 `STANDARD_PATTERNS`
  (`bindings/c/perf/run_benchmarks.sh:184`)와 계획서 §9.1.1 표(`tcp` 행)에서
  모두 첫 번째로 나열된 pattern이다.
- transport: `tcp` — 계획서 §3.1/진행 시트 3절 "Single은 `tcp`부터 선택한다"에
  따른 첫 transport.
- message size: `64` (smoke 고정값, §7.1)
- duration: `1`초, runs: `1` (smoke 고정값, §7.2)

## 2. 재현 환경 manifest

| 항목 | 값 |
|------|-----|
| host | `ulalax-gram` |
| OS/kernel | `Linux 6.6.87.2-microsoft-standard-WSL2` (WSL2) |
| CPU | `12th Gen Intel(R) Core(TM) i7-1260P`, 16 logical cores |
| CPU governor | 읽을 수 없음 (`/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` 없음 — WSL2에는 cpufreq 노출 안 됨) |
| 작업 브랜치 / commit | `codex/bindings-0.12.0-performance` / `9855b8f57d1b4f421ba54b2c860120bc07c85c68` |
| Core runtime 소스 | release (`ZLINK_CORE_SOURCE=release`, `local` 아님) |
| Core runtime prefix | `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64` |
| Core runtime 파일 | `lib/libzlink.so.0.12.0` |
| Core runtime sha256 | `aaff83d34ca1833d566feb39141ab5b4660d22429a551067de71b4f8967ba365` (release-provenance.txt/`core-package-provenance.json`과 일치, 별도 재검증) |
| Core release tag | `core/v0.12.0` |
| session tag | `bindings-0.12.0-smoke-20260823` |
| 시각(시작) | 2026-08-23 11:50 KST |

## 3. Core runtime 선택 메커니즘 (읽은 근거)

- `bindings/c/perf/run_benchmarks.sh`: `--core-version` 옵션이 없으면
  `ZLINK_CORE_SOURCE`를 **`local`로 강제**한다(49행:
  `export ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-local}"`). `--core-version
  0.12.0`을 주면 `ZLINK_CORE_SOURCE=release`,
  `ZLINK_CORE_RELEASE_VERSION=0.12.0`, `ZLINK_CORE_ALLOW_VERSION_MISMATCH=1`을
  자동 설정한다(38-44행). repo `VERSION`이 이미 `0.12.0`이므로 mismatch
  허용 플래그는 실질적으로 아무 효과가 없다(정확히 일치).
- `bindings/cpp/perf/run_binding_single.sh`: `--core-version` 옵션 자체가
  없다. `ZLINK_CORE_SOURCE`를 별도로 덮어쓰지 않으므로
  `bindings/tools/local_core_runtime.sh`의 기본값을 그대로 쓴다.
- `bindings/tools/local_core_runtime.sh`: `ZLINK_CORE_SOURCE`의 **기본값이
  `release`**다(8행: `ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-release}"`).
  `release` 모드에서는 `scripts/local-package/core/fetch-release.sh --version
  <VERSION>`을 호출해 `ZLINK_CORE_PACKAGE_PREFIX`를 얻고,
  `share/zlink/core-package-provenance.json`의 `version` 필드가
  `ZLINK_CORE_VERSION`과 일치하는지 검증한 뒤에만 통과시킨다.
  `fetch-release.sh`의 기본 cache root는
  `${XDG_CACHE_HOME:-$HOME/.cache}/zlink/core`이며, 이 호스트의 `$HOME`이
  `/home/hep7hep7`이므로 이미 검증돼 있는
  `/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64`를 재다운로드 없이 그대로
  재사용한다.
- **실제로 사용한 메커니즘**: C 러너는 `--core-version 0.12.0` 옵션을
  명시적으로 전달해 release 모드로 강제했다(그렇지 않으면 기본값이
  `local`이라 `core/build`의 로컬 빌드를 쓰게 된다 — 최초 시도에서 이 문제를
  발견하고 수정했다). C++ 러너는 옵션을 주지 않아도 기본값이 이미
  `release`이므로 추가 조치가 필요 없었다. 두 러너 모두 최종적으로 같은
  prefix(`/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64`)를 가리키는 것을
  C 실행 로그의 `PERF_CORE_RUNTIME`/`META,core_runtime` 필드로 확인했다.

## 4. 실행한 명령

### 4.1 C reference — 1차 시도(결함 발견, 기록용)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-smoke-20260823
```

`--core-version`을 주지 않아 `ZLINK_CORE_SOURCE=local`로 실행됐고
(`META,core_source,local`, `META,core_runtime,/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.12.0`),
release runtime 요구사항을 위반했다. 이 시도는 **release runtime을 쓰지
않았으므로 무효**이고, 곧이어 아래 4.2로 다시 실행했다. 결과 파일:
`bindings/c/perf/results/single/report/perf_c_single_linux_20260823_115009_bindings-0.12.0-smoke-20260823.txt`
(status: partial — 이것도 아래와 같은 이유로 timeout).

### 4.2 C reference — release runtime로 재시도(공식 시도)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-smoke-20260823
```

결과: **`status: partial`**, `PAIR current tcp 64B: timeout`.

- 결과 파일: `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_115107_bindings-0.12.0-smoke-20260823.txt`
- `META,core_source,release` / `META,core_runtime,/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0` 확인(요구한 release runtime 정확히 일치).
- `## Completion` 블록: `status: partial`, `expected_result_lines: 5`,
  `actual_result_lines: 0`.

### 4.3 진단 시도 (같은 실패가 pattern에 국한되지 않음을 확인)

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 3 --runs 1 \
  --results-tag diag-duration3
# -> 동일하게 timeout (duration을 늘려도 무관, 결과 파일
#    perf_c_single_linux_20260823_115201_diag-duration3.txt, status: partial)

PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --core-version 0.12.0 \
  --pattern ROUTER_ROUTER --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag diag-router
# -> 동일하게 timeout (pattern을 바꿔도 무관, 결과 파일
#    perf_c_single_linux_20260823_115411_diag-router.txt, status: partial)
```

바이너리 직접 실행(`PERF_DEBUG=1 ./perf_pair current tcp 64`, release
runtime `LD_LIBRARY_PATH`)으로도 재현했다: `Connected to tcp://127.0.0.1:<port>`
로그와 두 socket의 `SNDTIMEO`/`RCVTIMEO` 설정 로그까지는 출력되지만, 그 뒤
`setup_connected_pair()`의 `wait_for_socket_monitor_event(...,
ZLINK_EVENT_CONNECTION_READY, timeout_ms=1000)` 호출에서 더 진행하지 못하고
20초 `timeout` 명령으로 강제 종료(`exit 124`)될 때까지 아무 출력도 추가되지
않았다. `PERF_CONNECT_READY_TIMEOUT_MS` 기본값이 1000ms이므로 정상이라면
1초 안에 `bind_ready`/`connect_ready`가 결정되고(성공이든 실패 debug
로그든) 다음 단계로 넘어가야 하는데, 20초가 지나도 아무 진행이 없었다 —
`wait_for_socket_monitor_event`가 자신의 timeout을 지키지 못하고 무기한
대기하는 것으로 보인다.

OS 수준 TCP는 정상임을 별도로 확인했다:
- 실행 중인 `perf_pair`가 `127.0.0.1:<port>`에서 `LISTEN` 상태임을
  `ss -tlnp`로 확인.
- 같은 포트에 Python 원시 소켓으로 `connect()`가 즉시 성공.
- 별도 Python 스레드 서버/클라이언트로 loopback 데이터 송수신(`sendall`/
  `recv`)이 즉시 성공.
- `dangerouslyDisableSandbox=true`로 sandbox를 해제한 상태에서도 동일하게
  hang — 이 세션의 셸 sandbox가 원인이 아니다.

즉 실패는 **OS 수준 TCP나 이 실행 환경의 sandbox가 아니라, zlink core
0.12.0 release runtime의 TCP transport monitor(`ZLINK_EVENT_CONNECTION_READY`)
readiness 대기 경로**에서 발생한다. release runtime과 로컬(`core/build`)
runtime 양쪽에서 동일하게 재현되므로 release runtime에 국한된 문제도
아니다 — 이 WSL2 호스트에서 zlink core의 TCP transport 자체가 현재 동작하지
않는 것으로 보인다(inproc/ipc는 정상 동작 확인, §5 참조).

## 5. 참고: inproc/ipc는 정상 동작

같은 바이너리를 `inproc`/`ipc` transport로 직접 실행하면 정상적으로
`RESULT,...` 5줄을 출력하고 즉시 종료한다(수 밀리초 내):

```
$ LD_LIBRARY_PATH=<release-lib> ./perf_pair current inproc 64
RESULT,current,PAIR,inproc,64,throughput,2974007.600
...
$ LD_LIBRARY_PATH=<release-lib> ./perf_pair current ipc 64
RESULT,current,PAIR,ipc,64,throughput,2993982.000
...
```

이는 core runtime 자체나 빌드가 전반적으로 깨진 것이 아니라 TCP(그리고
아마 TCP 기반인 `ws`/`wss`/`tls`) transport의 connection-ready 신호 경로에
국한된 문제임을 시사한다. 과거 세션(`perf_c_single_linux_20260822_021410_
byte-hwm-single-rr-tcp-256-local.txt` 등)에는 tcp `RESULT` 값이 정상적으로
기록돼 있어, 이 hang은 **그 이전 세션 이후 이 워크스테이션의 상태가 바뀌었거나
(예: 커널/네트워스택 상태, WSL2 재시작 등) 이번 세션에 새로 발생한 문제**로
보인다. 원인이 코드 변경(diff)인지 호스트 상태인지는 이번 조사 범위에서
확정하지 못했다 — `git status`상 C 러너 관련 파일에는 변경이 없고, 이번
작업 지시에 명시된 우리 tree의 변경(C++ perf runner SPOT 제거,
`--sndbuf`/`--rcvbuf` 적용)은 C 쪽 코드에 영향이 없다.

## 6. C++ smoke 실행 여부

**실행하지 않았다.** 계획서 §7.1은 "C와 binding의 pattern별 smoke가 모두
`status: complete`여야 본 측정을 시작한다"고 규정하고, §7.3은 "C pattern
측정이 끝나면 다른 pattern을 실행하지 않고 바로 같은 binding pattern을
측정한다"고 규정한다. C reference가 `status: complete`에 도달하지 못했으므로
paired 기준이 없는 상태에서 C++ smoke를 실행하는 것은 계획서 규칙 위반이다.
C++ smoke는 C tcp hang 원인이 해소된 뒤 다시 시도한다.

## 7. 결과 요약

| 대상 | 명령 | 결과 파일 | status | 비고 |
|------|------|-----------|--------|------|
| C (local core, 무효) | §4.1 | `perf_c_single_linux_20260823_115009_bindings-0.12.0-smoke-20260823.txt` | partial | release runtime 아님 — 무효, 참고용 |
| C (release core, 공식) | §4.2 | `perf_c_single_linux_20260823_115107_bindings-0.12.0-smoke-20260823.txt` | **partial** | `PAIR current tcp 64B: timeout` |
| C 진단(duration=3) | §4.3 | `perf_c_single_linux_20260823_115201_diag-duration3.txt` | partial | 동일 timeout, duration 무관 확인용 |
| C 진단(ROUTER_ROUTER) | §4.3 | `perf_c_single_linux_20260823_115411_diag-router.txt` | partial | 동일 timeout, pattern 무관 확인용 |
| C++ (PAIR/tcp/64B) | 미실행 | 없음 | 해당 없음 | paired 기준(C complete)이 없어 시작하지 않음 |

**headline 수치는 없다.** 두 runner 모두 `status: complete`가 아니므로
throughput/latency 수치를 smoke 판단 근거로도, 판정값으로도 사용하지 않는다.

## 8. 진단(요약)과 다음 조치

- **진단**: 이 호스트(WSL2)에서 zlink core 0.12.0의 TCP transport가 연결은
  맺지만(`Connected to ...` 로그 확인, OS 레벨 connect도 별도 확인) connection
  monitor의 `ZLINK_EVENT_CONNECTION_READY` 대기(`wait_for_socket_monitor_event`,
  기본 timeout 1000ms)에서 자신의 timeout을 넘겨도 반환하지 않고 무기한
  block된다. `inproc`/`ipc`는 정상. release runtime과 local build 양쪽에서
  재현되므로 runtime provenance 문제가 아니다. 과거 세션에는 tcp가 동작한
  기록이 있어 회귀 또는 호스트 상태 변화로 추정되며, 이번 조사에서 근본
  원인(코드 회귀 vs 호스트/커널 상태)까지는 확정하지 못했다.
- **다음 조치(제안, 이번 작업 범위 밖)**: (1) core의 TCP transport
  connection-ready monitor 경로 또는 `wait_for_socket_monitor_event`의
  timeout 처리 코드를 원인으로 좁혀 조사, (2) 다른 호스트/세션에서 같은
  release runtime으로 재현되는지 교차 확인, (3) 원인 해소 후 이 문서의
  §4.2 명령을 그대로 다시 실행해 C가 `status: complete`가 되는지 재확인한
  뒤에만 C++ smoke를 진행.
- 이번 작업에서는 지시에 따라 재시도를 2회로 제한했고(§4.2 공식 시도 + §4.3
  진단 2회는 원인 범위를 좁히기 위한 것으로 반복 재시도가 아니다), 코드는
  전혀 수정하지 않았다.

## 로컬 core 재시도 (2026-08-23, 비공식)

> **이 절의 결과는 비공식(local core)이며 판정값이 아니다.** §7의 TCP
> deadlock 원인은 `log/2026-08-23-tcp-connection-ready-regression.md`에서
> 특정·수정됐고(commit `f99703c219`, "fix(transport): set Asio user
> non-blocking bit for sync write paths"), 그 문서 §5.1에서 이미 로컬 core
> 빌드로 PAIR/tcp/64B가 `status: complete`임을 확인했다. 이 절은 그 수정을
> **C++ perf 러너 end-to-end 검증** 관점에서 동일 조합으로 재확인한 기록이다.
> `core/v0.12.0` release asset은 결함 commit(`5d2bf1e84f`) 기준으로 빌드돼
> 여전히 무효 상태이며 재릴리스 대기 중이다(§7). 재릴리스된 release asset으로
> 같은 조합의 **공식(paired) smoke**를 다시 실행하기 전까지는, 이 절의 수치를
> §4의 진행 시트(progress.ko.md) §4 paired 측정 표나 계획서 상세 표에 반영하지
> 않는다.

### 전제 확인

- `bindings/c/perf/run_benchmarks.sh`, `bindings/cpp/perf/run_binding_single.sh`
  모두 이제 `--core-version`을 주지 않으면 기본값이 `local`이다(두 스크립트
  모두 `ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-local}"`). 이번 재시도는
  지시에 따라 `--core-version`을 전달하지 않고 두 러너 모두 기본값(local
  core)으로 실행했다.
- `core/build/lib/libzlink.so.0.12.0`의 mtime(`1787455715`,
  2026-08-23 12:28 KST)이 수정 소스 파일
  `core/src/runtime/transports/tcp/tcp_transport.cpp`의 mtime(`1787455705`,
  같은 시각 10초 전)보다 늦어 로컬 core 빌드가 수정을 반영한 최신 빌드임을
  확인했다(재빌드 불필요). 이는 tcp-fix 로그(§5.1, `..._122629_...` 등)를
  만든 빌드와 같다.

### 실행한 명령

```bash
# C reference (local core, 기본값)
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-localcore-retry-20260823

# C++ binding (local core, 기본값, 동일 조합)
PERF_FAIL_FAST=1 bash bindings/cpp/perf/run_binding_single.sh \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag bindings-0.12.0-localcore-retry-20260823
```

두 명령 모두 `--core-version`을 주지 않았으므로 기본값인 local core
(`core/build`)를 사용했다. C 러너 결과 파일의
`META,core_source,local` / `META,core_runtime,/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.12.0`
/ `META,core_revision,f99703c2190b0f6c670be49f67315d904886c742` /
`META,core_dirty,0`으로 이를 확인했다(C++ 러너의 report 파일에는 META 블록이
없으나, 같은 셸 세션에 `ZLINK_CORE_SOURCE` 환경변수 override가 없었고
스크립트 기본값이 `local`이므로 동일하게 local core를 사용했다).

### 결과

| 대상 | 결과 파일 | status | throughput | bandwidth | latency(ms) | latency p95/p99(ms) |
|------|-----------|--------|-----------:|----------:|------------:|---------------------|
| C (local core, 비공식) | `bindings/c/perf/results/single/report/perf_c_single_linux_20260823_123627_bindings-0.12.0-localcore-retry-20260823.txt` | **complete** | 2,655,581.000 msg/s | 169.957 MB/s | 50.091 | 53.725 / 55.898 |
| C++ (local core, 비공식) | `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260823_123634_bindings-0.12.0-localcore-retry-20260823.txt` | **complete** | 2,037,308.000 msg/s | 130.390 MB/s | 63.659 | 68.104 / 69.253 |

두 러너 모두 `expected_result_lines: 5`, `actual_result_lines: 5`,
`status: complete`로 종료했다. C++ smoke는 §6에서 처음 시도되지 못했던
단계를 이번에 통과했고, C++ perf 러너(수정 반영본 — SPOT 제거,
`--sndbuf`/`--rcvbuf` 배선)가 로컬 core 대상으로 정상적으로 end-to-end
동작함을 확인했다. 이번 재시도에서 새로 드러난 C++ 러너 결함은 없다.

### C++/C 비율 (비공식, 참고용)

- throughput 비율(C++ / C) = 2,037,308.000 / 2,655,581.000 = **0.767** (C++가 C
  대비 약 76.7%)
- latency 비율(C++ / C, mean) = 63.659 / 50.091 = **1.271** (C++ latency가 C
  대비 약 27.1% 더 높음)

이 비율은 **로컬 core 빌드** 기준이며, release core와 성능 특성이 다를 수
있다(§6의 sync write 관찰 참고). 판정값으로 사용하지 않는다.

### 다음 조치

- `core/v0.12.0` release asset이 수정 반영본으로 재릴리스되면, 같은 조합
  (`PAIR`/`tcp`/64B)의 **공식** paired smoke를 `--core-version 0.12.0`으로
  다시 실행해 `status: complete` 여부와 release runtime의 provenance
  (sha256/tag)를 확인한 뒤에만 progress.ko.md §4 paired 측정 표와 계획서
  상세 표에 기록한다.
- 이 절의 실행으로 코드는 수정하지 않았고, commit·push도 하지 않았다.
