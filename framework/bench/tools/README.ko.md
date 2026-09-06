# with-grpc bench 공용 집계기

측정은 언어별 harness가 하고, 측정 결과에서 나오는 모든 값은 이 도구가 만든다. 표, 단위
정규화, 중앙값, 재현성(G5), 규격 §7.2의 비율, 그리고 **그 비율을 게재해도 되는지에 대한 판정**이
전부 여기에 있다. 언어마다 집계 코드를 복제하면 표가 미묘하게 어긋나 비교가 깨지므로
(계획 §4.1) 언어 harness는 셀 원본만 낸다.

## 실행

```bash
python3 framework/bench/tools/bench_aggregate.py --lang dotnet \
    --runs-glob 'framework/languages/dotnet/bench/with-grpc/log/<stamp>/dotnet-*' \
    --runs-glob 'bindings/c/bench/with_grpc/log/<stamp>/c-*' \
    --json-out aggregate.json
```

한 번의 실행에 넘긴 run이 하나의 비교 집합을 이룬다. run마다 자기가 측정한 구현만 내놓으므로,
C 기준 run과 대상 언어 run을 함께 넘기면 `zlink-<lang> / zlink-c` 비율이 두 run을 가로지른다.

`--format`으로 구간을 고를 수 있다. `spec4`는 규격 §4 표, `result`는 `RESULT` 라인,
`judgement`는 판정표, `full`(기본값)은 전부다.

## 두 report 형식의 차이와 흡수 방법

| 항목 | `bindings/c/bench/with_grpc` | `framework/languages/*/bench/with-grpc` |
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

1. `cells.json` — 아래 구조화 형식. 단위 추정도 문장 파싱도 필요 없다.
2. `results.json` — `metadata.diagnosticsSchema`가 `with-grpc-cell-v1`이면 진단값을 여기서 읽는다.
   측정값은 그대로 `report.txt`의 `RESULT` 라인에서 읽는다.
3. stdout의 `[bench]` 라인 — FB-021 이전 출력에만 쓰는 fallback이다. 게재 여부를 결정하는 값을
   사람이 읽는 문장에서 되꺼내는 것은 전달 방식이 아니다.

`.NET` harness는 1·2를 만족하도록 고쳤고, `report.txt`의 옵션 머리글에 `logical_cores`와
`client_parallelism_ceiling`을 남긴다. 상한을 선언하지 않은 옛 결과는 포화를 **판정하지 않은**
것으로 표시하며(`not judged`), 판정을 막지도 않는다. 막으면 FB-019 이전 결과가 전부 소급해서
게재 불가가 된다.

## 새 언어가 낼 셀 원본 (`cells.json`)

run 디렉터리에 `cells.json`이 있으면 집계기는 `report.txt` 대신 그것을 읽는다. 이 형식에서는
단위 추정도, 문장 파싱도 필요 없다.

```json
{
  "schema": "with-grpc-cell-v1",
  "cells": [
    {
      "implementation": "zlink-framework-node",
      "pattern": "request-window",
      "payload_size": 1024,
      "throughput_per_second": 3663.2,
      "bandwidth_mb_s": 3.751,
      "latency_mean_ms": 28.045,
      "latency_p95_ms": 104.709,
      "latency_p99_ms": 174.524,
      "client_cpu_percent": 3.7,
      "client_memory_mb": 195.4,
      "server_cpu_percent": 5.2,
      "server_memory_mb": 486.1,
      "client_cores": 0.98,
      "client_parallelism_ceiling": 1,
      "peak_in_flight": 100,
      "request_window": 100,
      "abandoned": 0,
      "drain_ms": 16674,
      "drain_bound_hit": false,
      "server_received_at_close": 228385,
      "contaminated": false,
      "contamination_reason": null
    }
  ]
}
```

`throughput_per_second`는 request 계열이 완료 수, `send-saturation`이 **server 수신 수**다
(규격 §5, G3). `server_received_at_close`가 없는 `send-saturation` 셀은 client 제출 수를 센
것으로 보고 판정에서 제외한다(FB-014).

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

## 실제 in-flight 깊이

처리량 × 평균 지연(Little's law)을 셀마다 낸다. Phase 0에서 설정 window 100에 대해 실제 깊이가
8이었고, 그 한 값이 판정 하나를 뒤집었다(FB-010, FB-016). 그래서 이 값은 주석이 아니라 열이다.
평균 지연을 재지 않은 셀(C bench의 send 셀은 `0.000`을 낸다)은 깊이를 `n/a`로 둔다.

## test

```bash
cd framework/bench/tools && python3 -m unittest discover -s tests -p 'test_*.py'
```

`tests/test_acceptance_gated2.py`가 Phase 0 원본(`tests/fixtures/gated2/`)을 집계기에 넣어
[`bench-dotnet-summary.ko.md`](../../../doc/plan/fw-bench-worklog/bench-dotnet-summary.ko.md)의
18셀, 행별 G5, 네 판정을 그대로 재현하는지 확인한다. `tests/test_normalization.py`는 Phase 0
원본에 없는 경로(오염, 포화, 단위 판별 실패)를 다룬다.
