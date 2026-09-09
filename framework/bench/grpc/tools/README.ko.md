# with-grpc bench 공용 집계기

측정은 언어별 harness가 하고, 측정 결과에서 나오는 모든 값은 이 도구가 만든다. 표, 단위
정규화, 중앙값, 재현성(G5), 규격 §7.2의 비율, 그리고 **그 비율을 게재해도 되는지에 대한 판정**이
전부 여기에 있다. 언어마다 집계 코드를 복제하면 표가 미묘하게 어긋나 비교가 깨지므로
(계획 §4.1) 언어 harness는 셀 원본만 낸다.

## 실행

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang dotnet \
    --runs-glob 'framework/bench/grpc/log/dotnet/<stamp>/dotnet-*' \
    --runs-glob 'framework/bench/grpc/log/c/<stamp>/c-*' \
    --json-out aggregate.json
```

한 번의 실행에 넘긴 run이 하나의 비교 집합을 이룬다. run마다 자기가 측정한 구현만 내놓으므로,
C 기준 run과 대상 언어 run을 함께 넘기면 `zlink-<lang> / zlink-c` 비율이 두 run을 가로지른다.

`--format`으로 구간을 고를 수 있다. `spec4`는 규격 §4 표, `result`는 `RESULT` 라인,
`judgement`는 판정표, `doc-table`은 `--lang`으로 고른 언어의 문서용 Markdown 표,
`full`(기본값)은 나머지 구간을 전부 출력한다. `spec4`와 `doc-table`의 process 열은 내부 metric
이름(`client_*`·`server_*`)과 관계없이 `Source`·`Target`으로 표시한다.

## 두 report 형식의 차이와 흡수 방법

| 항목 | `framework/bench/grpc/c` | `framework/bench/grpc/{dotnet,node,java,cpp}` |
|---|---|---|
| `throughput` raw 값 | KOPS | 초당 완료 수 |
| 패턴 | 5종(`request-saturation`, `send-blocking` 포함) | 규격 §2의 3종 |
| 추가 열 | `Submitted`·`Completed`·`Errors`·`Blocked`·`MaxOut`·`SubmitMs` | 없음 |
| CPU 열 이름 | `C.CPU%`·`S.CPU%` | `Client CPU`·`Server CPU` |
| server 수신 수 | 없음 | `server_received_at_close` |
| 깊이·drain 계측 | 없음 | stdout `[bench]` 라인 |

두 형식이 공통으로 내는 것은 `RESULT` 라인뿐이므로 그것을 교환 형식으로 쓴다. 표의 열 이름
차이는 `RESULT` 라인을 읽는 것만으로 사라진다.

**단위는 설정하지 않고 계산해서 알아낸다.** `RESULT` 라인의 `throughput` 값이 어느 단위인지
어디에도 적혀 있지 않지만, `bandwidth`는 규격 §5가 MB/s로 고정한다. 그래서
`bandwidth × 10^6 ÷ payload_size`는 어느 runner가 썼든 초당 완료 수이고, 이 값을 보고된
`throughput`으로 나누면 그 runner가 쓴 배율이 나온다. Phase 0 원본에서 두 집단은 각각
1.0000과 1000.0에 모인다(margin 100배). 배율이 두 후보 중 어느 쪽도 아니거나 한 report 안에서
엇갈리면 report를 거부한다. 추측으로 메우지 않는다.

깊이·drain·오염 표시와 spec §5.1의 core 수·선언 상한은 **자료로 전달한다**(FB-021). 집계기가
run 디렉터리를 읽는 순서는 다음과 같다.

1. run 디렉터리 바로 아래의 구조화 JSON — 최상위 `schema`가 `with-grpc-cell-v1`인 `cells`
   문서와, 규격 §4의 `role`·`trigger`가 최상위에 있는 셀별 `results` 문서를 모두 읽는다.
   source와 target이 여러 파일을 쓸 수 있지만 파일 이름은 병합 기준이 아니다.
2. `results.json` — `metadata.diagnosticsSchema`가 `with-grpc-cell-v1`이면 진단값을 여기서 읽는다.
   측정값은 그대로 `report.txt`의 `RESULT` 라인에서 읽는다.
3. stdout의 `[bench]` 라인 — FB-021 이전 출력에만 쓰는 fallback이다. 게재 여부를 결정하는 값을
   사람이 읽는 문장에서 되꺼내는 것은 전달 방식이 아니다.

`report.txt`의 옵션 머리글에 `logical_cores`와 `client_parallelism_ceiling`을 남긴 원본은 이 값을
포화 판정에 사용한다. 상한을 선언하지 않은 옛 결과는 포화를 **판정하지 않은**
것으로 표시하며(`not judged`), 판정을 막지도 않는다. 막으면 FB-019 이전 결과가 전부 소급해서
게재 불가가 된다.

## 셀 원본과 server-driven 병합

`role`이 없는 `with-grpc-cell-v1` 레코드는 기존 client-driven 원본으로 읽는다. `role`이 있는
레코드는 규격 §4의 server-driven 형식이다. 기준 필드 이름을 바꾸거나 별칭을 만들지 않는다.
source와 target이 별도 파일을 쓰더라도 집계기는 `trigger.runId`와 `trigger.cellId` 두 값이 모두
같을 때만 한 셀로 합친다.

```json
{
  "schema": "with-grpc-cell-v1",
  "cells": [
    {
      "implementation": "zlink-framework-node",
      "pattern": "request-window",
      "payload_size": 1024,
      "role": "source",
      "trigger": {
        "runId": "run-01",
        "cellId": "framework-window-1024",
        "pattern": "request-window",
        "payloadBytes": 1024,
        "durationMs": 5000,
        "warmup": 1000,
        "endpoint": "http://127.0.0.1:5212/bench/start",
        "receivedAtUnixMs": 1788937000000
      },
      "streams": {"count": 1, "inFlightPerStream": 100},
      "throughput_per_second": 3663.2,
      "bandwidth_mb_s": 3.751,
      "latency_mean_ms": 28.045,
      "latency_p95_ms": 104.709,
      "latency_p99_ms": 174.524,
      "client_cpu_percent": 3.7,
      "client_memory_mb": 195.4,
      "client_cores": 0.98,
      "client_parallelism_ceiling": 1,
      "peak_in_flight": 100,
      "request_window": 100,
      "abandoned": 0
    },
    {
      "implementation": "zlink-framework-node",
      "pattern": "request-window",
      "payload_size": 1024,
      "role": "target",
      "trigger": {
        "runId": "run-01",
        "cellId": "framework-window-1024",
        "pattern": "request-window",
        "payloadBytes": 1024,
        "durationMs": 5000,
        "warmup": 1000,
        "endpoint": "http://127.0.0.1:5212/bench/start",
        "receivedAtUnixMs": 1788937000000
      },
      "server_cpu_percent": 5.2,
      "server_memory_mb": 486.1,
      "target_stats": {"received": 18316, "errors": 0, "drainMs": 16674}
    }
  ]
}
```

병합 뒤 필드 소유권과 실패 처리는 다음 한 규칙으로 고정한다.

| 항목 | 소유 원본과 처리 |
|---|---|
| workload 값, source 자원, `trigger`, `streams` | source A |
| target 자원, `target_stats` | target B. runner가 이미 source 레코드에 합친 `target_stats`도 같은 값으로 읽음 |
| `send-saturation` 처리량·bandwidth | `target_stats.received / trigger.durationMs`로 계산. source 제출 수는 사용하지 않음 |
| source/target key 불일치, 같은 역할 중복 | 어느 원본을 택할 수 없으므로 입력 오류로 중단 |
| target 원본과 source에 합쳐진 `target_stats`가 모두 없음 | 셀을 `incomplete`로 남기고 중앙값·`RESULT`·판정에서 제외 |
| target 없이 target 레코드만 있음 | 같은 방식으로 `incomplete` 처리 |

`target_stats.errors` 또는 `abandoned`가 0이 아닌 셀은 규격 §5.2에 따라 처리량 판정에 쓰지 않는다.
완전한 `send-saturation` 셀은 target 수신 수를 가지므로 server-counted 조건도 함께 충족한다.

규격 §7.1의 trigger endpoint는 §4 `trigger.endpoint`에서 읽어 동반 정보 표에 낸다. 3-run의 값이
다르면 `mixed`, 없으면 `n/a`이며 §9 포트로 추측하지 않는다.

## 게재 조건 (이 도구가 기계적으로 강제하는 것)

Phase 0에서 그럴듯해 보이는 잘못된 수치가 두 번 나왔다. 세 번째를 막는 장치는, 원본 셀에서
판정까지 가는 경로가 `judge` 하나뿐이고 `judge`가 아래 조건 중 하나라도 어긋나면 값 대신
`unsupported`와 사유를 낸다는 것이다.

| 조건 | 근거 |
|---|---|
| 분자 행과 분모 행이 **모두** G5를 통과한다 | FB-011 |
| payload `1024`와 `4096`을 따로 판정하고, 둘 다 통과해야 그 언어가 통과다 | FB-005, 규격 §7.2 |
| 오염된 셀은 중앙값에도 판정에도 들어가지 않는다 | FB-008 |
| 사용한 core 수가 **선언한 client 병렬성 상한**의 0.95배에 이른 행은 처리량 우열 판정에 쓰지 않는다 | 규격 §5.1, G6, FB-019 |
| `send-saturation` 처리량이 server 수신 수여야 한다 | 규격 §5, G3 |
| run이 3회 미만이면 G5를 통과할 수 없다 | 계획 §6 |

`unsupported` 사유는 어느 쪽 행이 왜 막았는지와 그 행의 스프레드를 함께 적는다.

## client 포화 (규격 §5.1, FB-019)

포화는 백분율이 아니라 **사용한 core 수 대 선언한 상한**으로 판정한다. 백분율은 머신의 논리
core 전체에 대한 값이라, 논리 core 20개인 머신에서 단일 스레드 client가 자기 core를 다 써도
4.9%로 보인다. 고정 백분율 기준으로는 그 포화를 잡을 수 없고, Node 행이 client에 묶인 값을
포화 표시 없이 게재하게 된다.

core 수는 구조화 입력의 `client_cores`를 그대로 쓰고, 없으면 옵션 머리글의 `logical_cores`와
백분율로 계산한다. 둘 다 없으면 포화를 판정하지 않고 그 사실을 결과에 남긴다.

### 계측기는 언어가 선언한다 (FB-023 · FB-032 · FB-037)

집계기는 `client_saturation_metric`이 이름한 계측기를 그대로 읽는다. 지원하는 이름은 넷이다.

| 이름 | 선언 언어 | 재는 것 |
|---|---|---|
| `client_cores` | `.NET`, 선언이 없는 옛 결과 | 프로세스 전체 CPU ÷ 경과 시간 |
| `event_loop_utilization` | Node | `performance.eventLoopUtilization()` |
| `jvm_thread_cores` | Java, Kotlin | 제출 스레드의 `ThreadMXBean` CPU |
| `submit_thread_cores` | C++ | 제출·완료 드레인을 도는 application thread의 `CLOCK_THREAD_CPUTIME_ID` |

**프로세스 CPU는 ZLink client의 포화 계측기로 쓸 수 없다.** 이것은 언어 하나의 사정이 아니라
세 언어에서 서로 다른 이유로 같은 결론이 나온 캠페인 수준의 결과다. Node에서는 프로세스 CPU가
binding의 native I/O thread를 함께 셌고, Java에서는 GC와 JIT thread를 셌고, C++에서는 다시
binding I/O thread를 센다. 다섯 언어 중 넷이 프로세스 CPU가 아닌 것을 선언했다.

문제는 임계값이 아니라 **판정식이 나누는 두 행 사이의 비교 가능성**이다. 실측된 C++ 값이
그것을 가장 짧게 보여준다.

| 행 | 선언 계측기 | 프로세스 core |
|---|---|---|
| `zlink-cpp` request-window | 0.955 | **1.92** |
| `grpc-cpp` request-window | 0.695 | **0.698** |

두 행의 프로세스 core 차이는 client 런타임의 바쁨이 아니라 **어느 행이 Core를 링크하는가**다.
그 값으로 포화를 판정하면 표시가 그 사실을 보고하게 된다.

## 실제 in-flight 깊이

처리량 × 평균 지연(Little's law)을 셀마다 낸다. Phase 0에서 설정 window 100에 대해 실제 깊이가
8이었고, 그 한 값이 판정 하나를 뒤집었다(FB-010, FB-016). 그래서 이 값은 주석이 아니라 열이다.
평균 지연을 재지 않은 셀(C bench의 send 셀은 `0.000`을 낸다)은 깊이를 `n/a`로 둔다.

## test

```bash
cd framework/bench/grpc/tools && python3 -m unittest discover -s tests -p 'test_*.py'
```

`tests/test_acceptance_gated2.py`가 Phase 0 원본(`tests/fixtures/gated2/`)을 집계기에 넣어
1차 캠페인의 `.NET` 요약(2026-09-09 제거)의
18셀, 행별 G5, 네 판정을 그대로 재현하는지 확인한다. `tests/test_normalization.py`는 Phase 0
원본에 없는 경로(오염, 포화, 단위 판별 실패)를 다룬다. `tests/test_server_driven.py`와
`tests/fixtures/s2s/`는 별도 A/B 원본 병합, target 누락, 3-run 중앙값, client-driven 원본과의
혼합, `doc-table` 형식을 검증한다.
