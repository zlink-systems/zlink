# .NET·Rust·Python routed echo relay C 구조 정합 — 2026-09-08

> 상태: **Rust·Python 완료. .NET 구현과 tcp smoke는 완료했지만 tls 전수는
> RR 65,536 B의 별도 client admission drain 문제로 45/50만 통과했다.**
> 수정 범위는 `bindings/{dotnet,rust,python}/perf`의 relay server 경로뿐이다.
> Core·Framework·binding 라이브러리·정책/스펙/계획서는 수정하지 않았고 commit·push도 하지 않았다.

## 1. 실행 조건과 기준

- 기준 구조: C `drain_recv_and_relay`의 receive snapshot → 같은 turn의 pending flush와
  C++ `e0862e1e5c`의 pending FIFO → 앞 reply admission 완료 뒤 다음 reply 제출.
- 고정 Core:
  - `ZLINK_CORE_SOURCE=release`
  - `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`
- 각 perf 실행 전에 `bash scripts/perf/wait-for-idle-perf.sh`를 실행했고, load 5 이하에서
  다른 측정과 겹치지 않게 한 번에 하나씩 실행했다.
- .NET과 Rust의 0.17.2 첫 실행은 `--reuse-build` 없이 시작해 해당 multi 프로젝트의 전체
  실행 파일을 먼저 빌드했다. Core는 수정하거나 재빌드하지 않았다.

## 2. relay 공유 여부와 변경

| 언어 | DR·RR 공유 여부 | receive snapshot | admission 직렬화 |
|---|---|---|---|
| .NET | 두 pattern이 기존 `PerfMultiRoutedRelayServer`를 공유 | 소유권을 넘긴 `Received` 전체를 FIFO에 보관하고 다음 receive는 새 storage 사용 | `PendingReplySender`가 FIFO head 하나만 `Async()`로 제출하고 그 Task가 admission 완료된 뒤 다음 head 제출 |
| Rust | server entry는 분리되어 있으나 새 `perf_common::RoutedReplySender`를 공유 | `RoutingId`와 payload `Vec<u8>`를 FIFO에 보관 | active Future 하나만 poll하고 완료 뒤 다음 snapshot으로 진행 |
| Python | server entry는 분리되어 있으나 새 `perf_multi_common.RoutedReplySender`를 공유 | routing id와 payload를 immutable `bytes`로 복사해 FIFO에 보관 | sender Task 하나가 FIFO head의 `send_routed` admission을 await한 뒤 다음 snapshot 제출 |

세 구현 모두 enqueue 때 sender를 즉시 진행하므로 C의 수신 turn별 flush와 대응한다.
backpressure가 걸리면 현재 head snapshot을 그대로 유지하고 binding의 admission 완료 신호를
기다린다. `NOT_CONNECTED`/`NOT_FOUND`만 기존 stale-route 의미대로 종료 성공으로 분류하며,
그 밖의 제출 오류는 실패로 전파한다.

### 2.1 상한과 규칙 수

pending FIFO에는 **상한 숫자, app 고정 window, 새 timeout 또는 sleep을 넣지 않았다.** 기존
teardown timeout과 Python scheduler fairness quantum도 변경하지 않았다. 수정 전에는
“수신마다 독립 제출”과 “나중에 task/future를 reap·gather” 두 규칙이 있었고, 수정 후에는
**“FIFO head의 admission 완료 후 다음 head 제출” 한 규칙**만 남았다(2개 → 1개).

## 3. 검증 결과

### 3.1 .NET

| 검증 | report/명령 | 결과 |
|---|---|---|
| tcp SENDSEND 2종, 64 B, 1 s, runs 1 | `perf_dotnet_multi_linux_20260908_085805_d_bp24_tcp_final.txt` | **complete**, success 2 / fail 0 |
| tls SENDSEND 2종, 64·256·1024·4096·65536 B, 5 s, runs 5 | `perf_dotnet_multi_linux_20260908_090317_d_bp24_tls_final.txt` | **partial**, success 45 / fail 5, result 225/250 |
| 단위 테스트 | `bash bindings/dotnet/tests/run_tests.sh` | **통과**, tests 232/232, samples 7/7 |

TLS 실패는 5회 모두 `MULTI_ROUTER_ROUTER_SENDSEND/tls/65536` 한 셀이다. client log의
첫 실패는 `PerfMultiRouterRouterClient.cs:194,282`에서
`pending send admissions did not drain within 5000 ms`이며,
`PerfMultiAdmissionSignal.cs:109`의 `TimeoutException`으로 끝난다. 같은 run의 server log에는
`READY`와 Auto-HWM만 있고 relay 제출 오류는 없다. DR 65,536 B를 포함한 나머지 45개 run은
모두 성공했다.

진단 중 sender 계수는 measurement 종료 시 RR client가 echo 수신을 중단하고 pending send
admission만 기다리는 동안, server FIFO head도 reply admission backpressure를 기다리는 상태를
확인했다. 변경 전 HEAD runner도 같은 직접 셀을 complete하지 못했으며 당시에는 기존 무제한
server reply task가 먼저 status 2로 끝났다. client 종료/drain 경로 수정은 이번 server 대상
범위를 벗어나므로 우회용 상한·timeout·순서 변경을 추가하지 않았다.

### 3.2 Rust

| 검증 | report/명령 | 결과 |
|---|---|---|
| tcp SENDSEND 2종, 64 B, 1 s, runs 1 | `perf_rust_multi_linux_20260908_083017_d_bp24_tcp.txt` | **complete**, success 2 / fail 0 |
| tls SENDSEND 2종, 전 크기, 5 s, runs 5 | `perf_rust_multi_linux_20260908_083843_d_bp24_tls.txt` | **complete**, 10개 pattern/size 셀·result 50/50 |
| all-target type check | 고정 native dir의 `cargo check --all-targets` | **통과**(기존 warning만) |
| 단위 테스트 | `bash bindings/rust/tests/run_tests.sh` | **13/14 suite 통과**, 아래 기존 실패 1건 |

실패 suite는 `ownership_tests`의
`request_future_preserves_more_than_1024_reply_parts`다. 단독 재실행도 고정 Core의
`socket_base_api.cpp:1629` `released` assertion에서 SIGABRT로 동일하게 실패했다. relay 변경
경로와 무관하며 assertion이나 fixture를 완화하지 않았다. `cargo fmt --check`는 통과했다.

### 3.3 Python

| 검증 | report/명령 | 결과 |
|---|---|---|
| tcp SENDSEND 2종, 64 B, 1 s, runs 1 | `perf_python_multi_linux_20260908_084426_d_bp24_tcp.txt` | **complete**, success 2 / fail 0 |
| tls SENDSEND 2종, 전 크기, 5 s, runs 5 | `perf_python_multi_linux_20260908_085427_d_bp24_tls.txt` | **complete**, success 50 / fail 0, result 250/250 |
| 단위 테스트 대체 | `bash bindings/python/tests/run_tests.sh`; `python3 -m compileall -q bindings/python/perf/multi` | pytest 미설치(`/usr/bin/python3: No module named pytest`), **compileall 통과** |

## 4. 남은 판정 항목

- Rust·Python relay server는 요구된 tcp/tls 행렬을 모두 complete했다.
- .NET relay server의 무제한 reply 제출 문제는 제거됐고 DR 전 셀과 RR 64~4096 B가
  runs 5에서 통과했다. RR/TLS/65,536 B의 client teardown/drain 문제는 별도 결함으로
  분리해야 하며, 해결 전에는 .NET TLS 전수 검증을 complete로 판정할 수 없다.
- `git diff --check`, Rust `cargo fmt --check`, Python `compileall`은 최종 diff에서 통과했다.
