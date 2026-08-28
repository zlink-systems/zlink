# perf runner 정합성 문제 정리 (측정 후 적용)

> 작성: 2026-08-28
> 대상: core 0.14.0 bindings 성능 측정
>
> 각 언어의 perf runner 가 C reference 와 **다른 계약으로 작성된 지점**을 모은다.
> 측정을 진행하면서 발견되는 대로 이 문서에 누적하고, **전체 측정이 끝난 뒤 한 번에 적용**한다.
>
> 측정 도중에 runner 를 고치면 이미 측정한 셀과 이후 셀의 조건이 갈리므로,
> 측정이 아예 불가능한 경우가 아니면 **기록만 하고 진행**한다.

## 원칙

- C reference 가 **기준**이다. C runner 는 고치지 않는다.
  C 쪽 결함으로 보이면 근거만 남기고 별도로 판단한다.
- binding runner 를 C 와 **같은 의미의 작업**을 측정하도록 맞춘다.
- **수치를 좋게 만들기 위한 튜닝은 금지**다. 측정 대상 축소, 조건 완화,
  C 에는 없는 유리한 최적화 추가는 하지 않는다.
- 각 항목은 "C runner 는 무엇을 하고 binding runner 는 무엇을 다르게 하는가" 를 명시한다.

## 반복되는 유형

지금까지 발견된 것들은 대부분 아래 세 유형이다.
새 항목을 추가할 때 유형을 먼저 확인하면 같은 함정을 빨리 찾을 수 있다.

1. **transient backpressure 를 fatal 로 처리** — C 는 재시도하는데 binding 은 실패로 끝낸다
2. **측정 메시지 구성 불일치** — part 수가 다르거나 payload 구성이 다르다
3. **runner 제어 흐름 결함** — 이전 case 의 상태가 다음 case 로 새거나, pattern 목록이 어긋난다

## 항목

### 1. C++ — PUBSUB active publish 가 `EAGAIN` 을 fatal 로 처리 (적용 완료)

- **유형**: 1
- **증상**: `PUBSUB/ws`, `PUBSUB/wss` 가 `non_zero_exit_1` 로 실패.
  단독 재현 시 `pubsub: active phase failed received=299434`
- **C runner**: transient backpressure 를 1ms 뒤 재시도하고, **성공한 경우에만 sequence 를 증가**
- **C++ runner**: `EAGAIN` 을 fatal 로 처리해 즉시 종료
- **처리**: C 와 같은 재시도 계약으로 수정. stop token 도 공통 timeout 까지 재시도하도록 맞춤
- **상태**: **적용 완료** (2026-08-27 smoke 단계에서 수정)
- **파일**: `bindings/cpp/perf/single/common/perf_single_common.hpp`,
  `bindings/cpp/perf/single/src/perf_pubsub.cpp`

### 2. Java — ROUTER_ROUTER 가 1-part 로 전송 (적용 완료)

- **유형**: 2
- **증상**: receiver 가 메시지를 버림
- **C runner**: 측정 메시지를 2 part(payload + 빈 tail)로 전송
- **Java runner**: active 메시지를 1 part 로 전송
- **영향**: 그대로 뒀다면 **Java ROUTER_ROUTER 수치 자체가 무효**
- **상태**: **적용 완료** (2026-08-27 smoke 단계에서 수정)
- **파일**: `bindings/java/perf/single/Zlink.BindingBench/src/main/java/systems/zlink/perf/single/PerfRouterRouter.java`

### 3. Java — 이전 case 의 nonzero status 가 다음 case 로 전파 (적용 완료)

- **유형**: 3
- **증상**: 실패가 다음 성공 case 에 남아 결과가 왜곡되거나 가려짐
- **상태**: **적용 완료** (2026-08-27 smoke 단계에서 수정)
- **파일**: `bindings/java/perf/single/run_benchmarks.sh`

### 4. Java — `ALL` pattern 목록에 제거된 SPOT 포함 (적용 완료)

- **유형**: 3
- **처리**: C single 권위 목록과 같은 7개 pattern 으로 정렬
- **상태**: **적용 완료** (2026-08-27 smoke 단계에서 수정)
- **파일**: `bindings/java/perf/single/run_benchmarks.sh`

### 5. report META 누락 (적용 완료)

- **유형**: 3
- **증상**: `.NET`, `Node`, `Go`, `Rust`, `Python` report 에
  `META,core_source` / `core_version` / `commit` / `core_runtime` 이 기록되지 않음.
  C 와 C++ 도 `PERF_CORE_RUNTIME` 이 없었다.
- **영향**: 계획 문서 1절이 요구하는 provenance 가 없어 **어떤 Core 로 잰 수치인지 증명 불가**
- **상태**: **적용 완료** (2026-08-27 smoke 단계에서 수정)
- **파일**: 각 언어 `perf/.../run_benchmarks.*`, `bindings/python/perf/perf_report.py`,
  `bindings/dotnet/perf/single/run_emit.py`, `bindings/node/perf/single/run_benchmarks.ts`

### 6. .NET — PAIR sender 가 transient 오류를 예외로 종료 (적용 완료)

- **유형**: 1
- **증상**: 0.14.0 .NET Single 측정 중 발견
- **C runner**: transient `EAGAIN` / `EWOULDBLOCK` / `ETIMEDOUT` 를
  **새 timestamp 와 1ms 대기로 재시도**
- **.NET runner**: 같은 오류를 예외로 처리해 종료할 수 있음
- **영향**: 측정이 실패하거나, 재시도 없이 끝나 수치가 왜곡될 수 있음
- **처리 방향**: C 와 같은 재시도 계약으로 맞춘다.
  재시도 시 timestamp 를 새로 찍고 성공한 경우에만 sequence 를 올리는 것까지 동일하게 한다.
- **상태**: **적용 완료**. `PerfShared.IsTransientBackpressure`와 `EpochNs`는
  공통 perf 프로젝트에 있고, 2026-08-28 Release 빌드와 `PAIR/tcp/64B` smoke가
  complete였다. sequence는 send 성공 뒤에만 증가한다.
- **파일**: `bindings/dotnet/perf/single/` 아래 PAIR sender 경로

### 7. .NET — PAIR stop-token 전송이 `POLLOUT` 대기 (적용 완료)

- **유형**: 1
- **증상**: `[single-pair] stop-token send failed` 로 PAIR 측정 실패.
  0.14.0 .NET Single `tcp/PAIR` 이 **미측정**으로 남았다.
- **C runner**: `ZLINK_SEND_FLAGS_NONE` blocking send를 사용하고, socket timeout으로
  돌아온 transient 오류는 **1ms 간격 bounded retry**한다.
- **.NET runner**: `POLLOUT` 신호를 기다린다
- **영향**: 측정 종료 처리에서 막혀 PAIR 자체가 측정되지 않는다
- **처리 방향**: C 와 같은 bounded retry 로 맞춘다
- **상태**: **적용 완료**. `POLLOUT` 대기를 제거하고 `SendFlags.None` blocking send와
  1ms 간격 bounded retry를 사용한다. 2026-08-28 Release 빌드와 `PAIR/tcp/64B`
  smoke가 complete였다.
- **영향 셀**: .NET Single 전 transport 의 `PAIR`
- **파일**: `bindings/dotnet/perf/single/` 의 PAIR stop path

### 8. .NET — PUBSUB 이 C 와 다른 것을 측정 (미적용, 수치 무효)

- **유형**: 2 또는 3 (원인 미규명)
- **증상**: `tcp/PUBSUB` 에서 64KiB 이상 비율이 **482~553%**,
  동시에 **latency 164.4x**. aggregate 284.1%.
- **왜 무효인가**: 처리량이 C 의 5배인데 지연이 164배일 수는 없다.
  두 runner 가 **같은 의미의 작업을 측정하고 있지 않다**는 뜻이다.
- **작은 메시지는 43~45%** 로 오히려 낮아, size 에 따라 측정 대상이 갈리는 것으로 보인다
- **처리 방향**: C PUBSUB runner 와 .NET PUBSUB runner 의
  구독 시점, 측정 경계, drain 방식, 수신 확인 조건을 대조한다.
- **상태**: **미적용**. 이 셀의 수치는 **판정에 사용하지 않는다**
- **영향 셀**: .NET Single 전 transport 의 `PUBSUB` (다른 transport 도 같은지 확인 필요)

### 9. .NET — routed 계열 latency 이상치 (원인 미규명)

- **유형**: 미상
- **증상**: `tcp/ROUTER_ROUTER` **38.5x**, `tcp/DEALER_ROUTER` **29.2x**.
  .NET latency 상한은 3.0x 다.
- **비교**: C++ 에서도 routed 계열에 5~14x 가 산발적으로 나왔다.
  **두 언어 공통 현상**이라 runner 문제인지 core 문제인지 갈리지 않았다.
- **처리 방향**: 5회 median 재측정으로 drift 여부부터 확인한 뒤,
  재현되면 latency 측정 지점을 C 와 대조한다.
- **상태**: **미적용**

### 10. Multi DEALER/DEALER — C와 binding의 송신 포화 조건 불일치

- **유형**: I/O 계약 불일치
- **latency anchor**: C와 Java·Node·Go·Python은 모두 active payload에 송신 직전
  timestamp를 기록하고, server receiver가 유효 header를 받은 직후 `recv_ts - sent_ts`를
  계산한다. throughput도 모두 같은 server receive count를 active duration으로 나눈다.
- **차이**: C는 `ZLINK_DONTWAIT`로 HWM까지 연속 제출하고 `EAGAIN` 뒤 `POLLOUT`을
  기다린다. 네 binding의 현재 public routed-send terminal은 Core admission 완료를
  기다리며, one-way sender가 C와 같은 queue backlog를 만들 수 있는 nonblocking
  terminal을 제공하지 않는다.
- **판정**: 기존 수치 차이는 timestamp anchor 차이가 아니라 queue 포화 조건 차이다.
  sender가 receiver보다 느리면 throughput이 낮으면서 queue 체류 latency도 낮을 수 있다.
  2026-08-28 재실행에서도 C 64B latency가 0.336ms인 반면 Python은 348.107ms여서,
  throughput 비율만으로 latency 비율을 판정할 수 없음을 확인했다.
- **처리**: timestamp를 옮기거나 latency에 임의 대기 시간을 더하지 않았다.
  `SNDTIMEO=0`과 routed operation zero timeout도 Java 실험에서 같은 포화 상태를
  만들지 못했다. runner만 수정해서 C의 `DONTWAIT` 의미를 복원할 공개 계약이 없으므로,
  public routed-send 계약 설계와 함께 별도 처리해야 한다. Go 64B receiver가 mean까지
  32개마다 표본화하던 별도 정책 위반은 제거했다. 모든 유효 수신을 exact mean에 더하고,
  p95/p99만 65,536개 bounded reservoir에 보관한다.
- **상태**: **미해결 — binding public API 변경 필요**

### 11. C Multi DEALER/DEALER — 다중 pipe multipart tail 오판 (적용 완료)

- **유형**: C reference runner 결함
- **재현**: `tcp`, 100 clients, 64B, duration 1초를 연속 실행하면 수정 전 12회 중
  2회 `server_non_zero_exit_1`이 발생했다. 1 client에서는 30회 모두 성공했다.
- **원인**: shared DEALER server의 `zlink_recv_part`는 연결된 여러 pipe를
  multiplex한다. payload의 `has_more`가 설정되어도 다음 호출이 같은 pipe의 빈 tail을
  돌려준다는 보장이 없다. 다른 pipe의 19B stop token을 tail로 받아 fatal 처리했다.
  첫 64B case에서 server가 종료되므로 report가 같은 failure reason을 이후 size에
  복제했으며, 1KiB·64KiB는 실제로 실행되지 않았다.
- **처리**: 각 part를 독립적으로 drain하고 metric header가 일치하는 payload part만
  throughput과 latency에 포함한다. 빈 tail과 다른 non-metric part는 소비만 한다.
- **검증**: 수정 후 같은 100-client 조건에서 30회 연속 complete였다. C++ → .NET →
  Rust 순서로 세 size를 다시 실행한 C reference도 모두 complete였다.
- **throughput 영향**: 수정 전 성공 run 10회의 64B median은 895,822 msg/s,
  수정 후 30회의 median은 912,577 msg/s로 1.9% 차이다. count와 latency 계산식은
  바뀌지 않았다.
- **상태**: **적용 완료**
- **파일**: `bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp`

## 측정 후 적용 절차

1. 이 문서의 **미적용 항목**을 언어별로 나눠 수정한다.
   언어 디렉터리가 분리되어 있으므로 **언어별 agent 병렬 작업이 가능**하다.
2. 수정 후 해당 언어의 **smoke(64B, `--pattern ALL`)로 동작을 확인**한다.
3. 정정한 항목이 영향을 주는 셀을 **재측정**한다.
   어떤 셀이 영향을 받는지 각 항목에 기록해 둔다.
4. 재측정 결과로 계획 문서 표와 xlsx 를 갱신한다.

## 남은 확인 항목

측정 중 관찰됐으나 아직 원인이 규명되지 않은 것들이다. runner 문제인지 binding 문제인지
core 문제인지 갈리지 않았다.

- `MULTI_DEALER_DEALER` 이외의 C reference 산발 실패는 pattern별 원인 확인이
  필요하다. 이번 수정은 shared DEALER receiver가 multipart tail을 오판한 경로에만
  적용했다.
- **MULTI_PUBSUB 이 C 대비 164%** — C 보다 2배 이상 빠르다.
  측정 조건이 C 와 다를 가능성이 있어 확인이 필요하다.
- **inproc 대용량(64KiB~128KiB) 붕괴** — C++ 에서 23~41%.
  다른 transport 와 반대 방향이고 PUBSUB 만 예외적으로 통과했다.
  runs=1 이므로 **5회 median 재측정으로 실재 여부부터 확인**해야 한다.
- **routed 계열 latency 이상치** — 5~14x 가 산발적으로 나온다.
  runs=1 drift 로 의심되나 routed 계열에만 나타나는 공통점이 있다.
