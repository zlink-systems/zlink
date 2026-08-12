# Java TCP DEALER/DEALER receive state 결과

release Core `0.10.1`을 사용했고 C와 Java를 직렬로 실행했다. 조건은 `tcp`,
`MULTI_DEALER_DEALER`, message size `64,256,1024,4096,65536,131072`, duration 5초,
client 100, auto-HWM `balanced`다.

| Size | C throughput (msg/s) | Java throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,595,050.2 | 1,795,525.0 | 69.19% |
| 256 | 1,437,571.2 | 1,221,825.6 | 84.99% |
| 1,024 | 966,369.6 | 808,788.0 | 83.70% |
| 4,096 | 432,040.6 | 375,358.6 | 86.88% |
| 65,536 | 96,528.6 | 80,801.8 | 83.71% |
| 131,072 | 50,766.2 | 43,733.4 | 86.15% |
| 산술평균 | - | - | **82.44%** |

single-part `DONT_WAIT` 수신에서 이미 조회한 multipart state를 다시 `ThreadLocal`에서
가져오지 않도록 변경했다. 이전 확인값 81.32%보다 1.12%p 상승했지만 목표 90%에는 미달한다.

- C: `/tmp/zlink-java-recv-state-c/multi/report/perf_c_multi_linux_20260813_020354_java-recv-state-c.txt`
- Java: `/tmp/zlink-java-recv-state-java/multi/report/perf_java_multi_linux_20260813_020435_java-recv-state-java.txt`

## 채택한 receive·poll contract bridge cache

`ReceivePlane`과 `NativePoller`는 수신 frame과 ready event를 처리할 때마다
`ContractAccess`의 volatile bridge를 다시 조회했다. runtime class 초기화에서 bridge를
한 번 해석해 static final field에 보관하고, 이후에는 caller-provided `Received`와
`PollEvents`에 직접 채우도록 변경했다. 공개 interface와 perf harness는 변경하지 않았다.

tcp `MULTI_DEALER_DEALER`, 100 clients, auto-HWM `balanced`, I/O thread 4,
release Core 0.10.1, duration 2초에서 C 다음 Java를 실행했다.

| Size | C throughput (msg/s) | Java throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,376,163.0 | 1,490,746.0 | 62.74% |
| 256 | 1,383,367.5 | 1,110,396.5 | 80.27% |
| 1,024 | 949,024.0 | 863,486.5 | 90.99% |
| 4,096 | 394,999.0 | 250,549.5 | 63.43% |
| 65,536 | 112,927.5 | 70,706.0 | 62.61% |
| 131,072 | 50,612.5 | 35,472.5 | 70.09% |
| 산술평균 | - | - | **71.69%** |

같은 2초 조건의 변경 전 확인값 60.57%보다 11.12%p 상승했다. 목표 90%에는 미달하므로
single-part receive의 FFM 호출과 message wrapper 상태를 다음 후보로 검토한다.

- C: `/tmp/zlink-java-dd-c-2s/multi/report/perf_c_multi_linux_20260813_040313_java-dd-c-2s.txt`
- Java: `/tmp/zlink-java-dd-access-cache-java/multi/report/perf_java_multi_linux_20260813_040646_java-dd-access-cache.txt`

## 채택한 native message·runtime message bridge cache

수신 wrapper는 native `init`, `size`, `data`, `close`와 message wrapper accessor를
매 frame마다 `ContractAccess`를 통해 조회했다. `Message`와 runtime `InternalAccess`가
class 초기화 때 각각 native/message accessor를 static final field에 보관하도록 변경했다.
이는 message ownership, public `Message` API, Core ABI를 변경하지 않는다.

같은 C baseline과 Java 2초 실행에서 C 대비 throughput은
`65.10 / 84.02 / 97.34 / 71.44 / 83.08 / 97.51%`, 산술평균은 `83.07%`다. 직전
bridge cache 측정 평균 `71.69%`보다 높다. Java build와 multi perf compile도 통과했다.

- C: `/tmp/zlink-java-dd-c-2s/multi/report/perf_c_multi_linux_20260813_040313_java-dd-c-2s.txt`
- Java: `/tmp/zlink-java-dd-message-access-java/multi/report/perf_java_multi_linux_20260813_041635_java-dd-message-access.txt`

single-part `Received`의 pooled list를 제거하는 후보도 검토했다. 현재 list는
caller-provided storage의 close·adopt·multipart transition을 함께 소유하며, access할 때
fast path를 이미 사용한다. 이를 없애면 ownership 경계를 크게 다시 설계해야 하고 이
대상에 비례한 작은 개선으로 판단할 수 없어 채택하지 않았다.
