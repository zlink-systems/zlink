# Cross-language focused measurement — 2026-08-28

7개 binding의 공통 병목인지, 특정 binding의 문제인지 가리기 위한 축소 측정이다. 공식 전수 측정이나 통과 판정에는 사용하지 않는다.

## 조건

- 순서: `cpp` → `dotnet` → `java` → `node` → `go` → `rust` → `python`. 각 suite에서 C reference를 먼저 실행한 직후 binding을 실행했다.
- runtime: `ZLINK_CORE_SOURCE=local`, `--core-version` 미사용, `runs=1`, `duration=5`, `transport=tcp`.
- size: 64B, 1KiB, 64KiB만 실행했다.
- Single: `DEALER_ROUTER_REQREP`; Multi: `MULTI_DEALER_DEALER`.
- Java 실행에는 `JAVA_HOME=/home/hep7hep7/.jdks/jdk-22.0.2+9`를 명시했다.
- 비율은 같은 언어의 바로 앞 C reference 대비 binding throughput이다. aggregate는 세 size 비율의 산술 평균이고, latency 중앙값은 세 size의 `binding latency / C latency` 중앙값이다.

## 결과

| 언어 | suite | pattern | 64 비율 | 1KiB 비율 | 64KiB 비율 | aggregate | latency 중앙값 |
|---|---|---|---:|---:|---:|---:|---:|
| C++ | Single | `DEALER_ROUTER_REQREP` | 54.1% | 36.6% | 74.9% | 55.2% | 1.71x |
| C++ | Multi | `MULTI_DEALER_DEALER` | C 실패 | C 실패 | C 실패 | 산출 불가 | 산출 불가 |
| .NET | Single | `DEALER_ROUTER_REQREP` | 32.0% | 33.7% | 101.8% | 55.8% | 2.01x |
| .NET | Multi | `MULTI_DEALER_DEALER` | C 실패 | C 실패 | C 실패 | 산출 불가 | 산출 불가 |
| Java | Single | `DEALER_ROUTER_REQREP` | 32.7% | 38.2% | 75.8% | 48.9% | 2.08x |
| Java | Multi | `MULTI_DEALER_DEALER` | 46.9% | 48.8% | 38.8% | 44.8% | 0.004x |
| Node | Single | `DEALER_ROUTER_REQREP` | 17.7% | 16.2% | 32.0% | 22.0% | 6.31x |
| Node | Multi | `MULTI_DEALER_DEALER` | 14.6% | 1.4% | 17.6% | 11.2% | 0.026x |
| Go | Single | `DEALER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |
| Go | Multi | `MULTI_DEALER_DEALER` | 44.6% | 46.4% | 50.6% | 47.2% | 0.032x |
| Rust | Single | `DEALER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |
| Rust | Multi | `MULTI_DEALER_DEALER` | C 실패 | C 실패 | C 실패 | 산출 불가 | 산출 불가 |
| Python | Single | `DEALER_ROUTER_REQREP` | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 | 해당 없음 |
| Python | Multi | `MULTI_DEALER_DEALER` | 15.0% | 17.4% | 60.3% | 30.9% | 0.005x |

## 관찰

### 언어 공통 현상

- 비교 가능한 Single 네 언어(C++, .NET, Java, Node)는 모두 64B와 1KiB에서 C보다 크게 낮았다. C++·.NET·Java는 64KiB에서 비율이 올라갔지만, Node는 32.0%에 머물렀다.
- 비교 가능한 Multi 네 언어(Java, Node, Go, Python)는 세 size 모두 C throughput에 못 미쳤다. aggregate도 11.2%~47.2%다. 작은 payload의 격차가 공통으로 크다.

### 특정 언어 현상

- Node는 Single과 Multi 모두 최저이며, Multi 1KiB는 1.4%까지 떨어졌다.
- .NET Single 64KiB만 C를 101.8%로 넘었다. 나머지 두 size는 약 32%다.
- Python Multi는 64KiB에서 60.3%로 회복하지만 64B·1KiB는 20% 아래다.
- Go Single, Rust Single, Python Single은 지정 pattern runner가 없다.

## 측정 불가·실패

- Go Single은 `DEALER_ROUTER_REQREP` binary가 없어 첫 64B에서 `exit_nonzero`가 났다.
- Rust Single은 지정 pattern에 대해 `expected_result_lines: 0`으로 정상 종료했다. 등록된 측정이 아니므로 해당 없음으로 기록했다.
- Python Single은 `unsupported pattern: DEALER_ROUTER_REQREP`로 종료했다.
- C++·.NET·Rust Multi의 C reference는 64B, 1KiB, 64KiB 모두 `server_non_zero_exit_1`로 실패했다. 같은 명령이 Java·Node·Go·Python 차례에서는 complete였으므로 binding 실패로 분류하지 않았다.

## C와 binding runner의 metric 의미가 다를 수 있는 지점

- Multi 성공 pair에서 C latency는 0.749~79.307ms, binding latency는 0.087~1.102ms로 차이가 매우 크다. Java와 Python의 latency 비율은 특히 0.004x~0.005x다. throughput 비율과 독립적으로 latency 수집 범위, timestamp 위치 또는 aggregation이 C와 binding에서 같은지 확인이 필요하다.
- C Multi 실패 세 run은 1KiB·64KiB에도 `msg_size=64`인 `AUTO_HWM_DETAIL`만 남겼다. 실패 원인 진단이 실제 case size를 가리키지 않아, C runner의 failure diagnostic이 case별 상태를 정확히 나타내는지도 확인이 필요하다.

## local Core native 동기화 확인

- Core commit: `510f161e8f`; runtime: `core/build/lib/libzlink.so.0.14.0`.
- 측정 직전 각 언어에 `scripts/local-package/native/sync-local-core-libs.sh <lang>`를 실행했다.
- Core runtime과 각 언어 native copy의 SHA-256은 모두 `a6f7a7fb727b7e1e05cc9a7f088376af5a5c34e0fcbc34bc2601b9674b077777`로 일치했다. C++·Java·Node·Go·Rust는 3개 copy, .NET·Python은 2개 copy를 대조했다.

원시 runner 출력은 `/tmp/zlink-focused-20260828/`에 보관했다. 각 runner report는 `bindings/*/perf/results/{single,multi}/report/`의 `*focused-20260828-*` 파일이다.

## MULTI_DEALER_DEALER 재측정 — 2026-08-28

local Core, `tcp`, `runs=1`, `duration=5`, 100 clients 조건으로 C를 먼저 실행한 뒤
Java, Node, Go, Python을 실행했다. 값은 `throughput msg/s / latency mean ms`다.

| runner | 64B | 1KiB | 64KiB | 상태 |
|---|---:|---:|---:|---|
| C | 985,062.0 / 0.336 | 879,443.2 / 1.768 | 108,874.2 / 70.254 | complete |
| Java | 433,728.2 / 0.155 | 391,090.8 / 0.388 | 43,158.8 / 0.245 | complete |
| Node | 25,726.0 / 0.208 | 90,359.8 / 0.261 | 13,827.0 / 0.940 | complete |
| Go | 374,228.2 / 13.087 | 394,899.4 / 0.414 | 7,057.2 / 0.411 | complete |
| Python | 106,758.6 / 348.107 | 136,903.8 / 3.431 | 49,291.4 / 0.419 | complete |

timestamp와 receive anchor는 C와 네 binding이 같았다. run별 queue backlog가 달라 latency가
throughput 비율과 함께 움직이지 않았고, 같은 size에서도 C보다 낮거나 높은 값이 모두
나왔다. 따라서 기존 latency 비율만으로 anchor 불일치를 판정하지 않는다. C의 one-way
sender는 `DONTWAIT → EAGAIN → POLLOUT`으로 queue를 채우지만, 네 binding의 public
routed-send terminal은 admission 완료를 기다린다는 I/O 계약 차이는 남아 있다.

### C reference 종료 경로 재검증

shared DEALER socket에서 payload 다음 `recv_part`가 같은 pipe의 빈 tail이라는 가정을
제거했다. 수정 전에는 100 clients·64B·1초 연속 실행 12회 중 2회 실패했고, 1 client는
30회 모두 성공했다. 수정 후 100 clients 조건에서 30회 모두 complete였다. 이어서
C++ → .NET → Rust 차례를 대신해 C reference를 같은 순서로 세 번 실행했으며, 매번
64B·1KiB·64KiB가 모두 complete였다.

수정 전 성공 run 10회의 64B throughput median은 895,822 msg/s, 수정 후 30회의
median은 912,577 msg/s였다. 1.9% 차이로 기존 run 변동 범위 안이며, receiver의 metric
header count와 `recv_ts - sent_ts` 계산은 변경하지 않았다.
