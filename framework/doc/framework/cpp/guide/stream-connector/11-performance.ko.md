# 11 — 성능 테스트

[← 패키징](10-packaging.ko.md) | [목차](INDEX.ko.md)

---

`connector_perf_client`는 많은 수의 일반 client가 connector를 통해 request/wait 흐름을 동시에 수행하는지 검증하는 tool이다. server orchestration은 runner script나 CTest fixture가 담당하고, perf executable은 부하 생성과 report 작성만 담당한다.

---

## 빌드

```bash
cmake --build framework/languages/cpp/build --target connector_perf_client
```

---

## Smoke — CI 구조 회귀

smoke는 10 client, 2 worker, 5초 loopback 설정으로 external server 없이 실행한다.

```bash
cmake --build framework/languages/cpp/build --target connector_perf_smoke
ctest --test-dir framework/languages/cpp/build -L connector-perf-smoke --output-on-failure
```

smoke는 다음을 확인한다.
- `co_await request().async()`, `co_await wait_for().async()` 흐름이 동작한다.
- JSON report가 생성되고 schema가 올바르다.
- worker thread가 client 수와 무관하게 고정된다.

---

## Scale — 5천 client 측정

전용 STREAM test server endpoint가 필요하다.

```bash
framework/languages/cpp/build/connector_perf_client \
  --clients       5000 \
  --workers       4 \
  --duration      60s \
  --warmup        10s \
  --request-timeout-ms 1000 \
  --transport     tcp \
  --dispatch-mode immediate \
  --endpoint      tcp://game-test-server.internal:7000 \
  --report        /tmp/connector-perf-5000.json
```

### 옵션

| 옵션 | 의미 | 기본값 |
|------|------|--------|
| `--clients` | 동시 connector 수 | `5000` |
| `--workers` | Asio worker thread 수 | `4` |
| `--duration` | 측정 시간 | `60s` |
| `--warmup` | warmup 시간 | `10s` |
| `--payload-bytes` | request payload 크기 | `256` |
| `--inflight` | client당 동시 request 수 | `1` |
| `--wait-clients-percent` | push notification wait를 함께 등록하는 client 비율 | `10` |
| `--transport` | `tcp`, `tls`, `ws`, `wss` | `tcp` |
| `--dispatch-mode` | `manual`, `immediate` | `immediate` |
| `--request-timeout-ms` | request timeout | `1000` |
| `--endpoint` | 서버 주소 | 필수 (loopback smoke 제외) |
| `--report` | JSON report 경로 | build dir 아래 자동 생성 |

`--workers`는 report에만 기록되는 값이 아니다. perf client는 connector를 만들기 전에 shared connector runtime의 worker thread 수를 이 값으로 설정한다.

---

## Report 형식

```json
{
  "clients":          5000,
  "workers":          4,
  "duration_ms":      60000,
  "requests_total":   2847361,
  "throughput_rps":   47456,
  "latency_p50_us":   412,
  "latency_p95_us":   1823,
  "latency_p99_us":   4201,
  "timeouts_total":   3,
  "errors_total":     0,
  "rss_bytes":        184549376,
  "cpu_user_ms":      87432,
  "cpu_system_ms":    14211
}
```

---

## 회귀 판단 기준

hardware 차이가 크기 때문에 절대 수치만으로 실패시키지 않는다. 다음 항목은 회귀로 판단한다.

| 항목 | 회귀 신호 |
|------|-----------|
| thread 수 | client 수에 비례해 thread가 증가한다 |
| blocking | async request 중 worker thread가 blocking wait에 묶인다 |
| latency | 같은 환경 기준값보다 p99가 지정 비율 이상 악화된다 |
| timeout/error | timeout/error 비율이 threshold를 넘는다 |
| RSS | client 수 대비 RSS가 비정상적으로 증가한다 |

---

## runner script

```bash
framework/languages/cpp/connector/perf/run_connector_perf.sh \
  --clients 5000 \
  --workers 4 \
  --endpoint tcp://game-test-server.internal:7000
```

runner script는 server 시작, perf client 실행, report 수집을 담당한다. 실행 결과는 PR description이나 성능 리포트에 실행 환경, CPU, OS, compiler, build type과 함께 기록한다.

---

## CTest label

| label | 내용 |
|-------|------|
| `connector-perf-smoke` | 10 client loopback smoke |
| `connector-perf-scale` | 5천 client scale (전용 환경) |

```bash
ctest --test-dir framework/languages/cpp/build \
  -L connector-perf-smoke \
  --output-on-failure
```
