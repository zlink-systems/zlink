# .NET PUB/SUB tcp 측정 기록

tcp `MULTI_PUBSUB`을 C 다음 .NET 순서로 한 번씩 실행했다. 두 실행은 100 clients,
5초 active duration, `64/256/1024/4096/65536/131072B`, auto-HWM balanced, I/O thread
4, 기본 socket buffer와 release Core 0.10.1을 사용했다.

| Size | C throughput | .NET throughput | C 대비 |
|---:|---:|---:|---:|
| 64B | 1,725,400.8 msg/s | 982,051.6 msg/s | 56.92% |
| 256B | 1,793,875.4 msg/s | 805,143.8 msg/s | 44.88% |
| 1024B | 1,527,808.0 msg/s | 759,309.8 msg/s | 49.70% |
| 4096B | 621,734.4 msg/s | 412,327.0 msg/s | 66.32% |
| 65536B | 116,149.6 msg/s | 105,091.2 msg/s | 90.48% |
| 131072B | 54,991.6 msg/s | 55,007.0 msg/s | 100.03% |

산술평균은 `68.05%`이며 목표 `85%`에 미달한다.

- C: `/tmp/zlink-dotnet-pub-current-c/multi/report/perf_c_multi_linux_20260813_010544_dotnet-pub-current-c.txt`
- .NET: `/tmp/zlink-dotnet-pub-current-dotnet/multi/report/perf_dotnet_multi_linux_20260813_010619_dotnet-pub-current-dotnet.txt`

## 채택한 single-part publish lock 정리

`Publish`와 `Publish(DontWait)`의 topic cache 확인과 native submit이 각각
`SubmitGate`를 잠그던 경로를 한 lock 범위로 합쳤다. validation, topic cache, message
소유권과 public builder interface는 바꾸지 않았다. 같은 조건의 C 다음 .NET 실행에서
C 대비 throughput은 `62.47 / 50.28 / 53.53 / 75.87 / 87.79 / 109.73%`, 산술평균은
`73.28%`였다. 이전 평균 `68.05%`보다 높아 채택한다.

- C: `/tmp/zlink-dotnet-pub-singlelock-c/multi/report/perf_c_multi_linux_20260813_012009_dotnet-pub-singlelock-c.txt`
- .NET: `/tmp/zlink-dotnet-pub-singlelock-dotnet/multi/report/perf_dotnet_multi_linux_20260813_012046_dotnet-pub-singlelock-dotnet.txt`

## 채택한 TopicMessage two-slot 수신 wrapper

caller-provided `TopicMessage`에서 공개 중인 single-part와 다음 native receive에만 쓰는
private candidate를 분리했다. successful single-part receive에서만 두 wrapper를 교대하고,
`DONT_WAIT` EAGAIN에서는 기존 result를 변경하지 않는다. multipart 수신과 public interface는
그대로 유지했다. 같은 조건의 C 다음 .NET 실행에서 C 대비 throughput은
`66.58 / 60.71 / 57.22 / 76.19 / 110.03 / 103.58%`, 산술평균은 `79.05%`다.
이전 평균 `73.28%`보다 높아 채택한다.

- C: `/tmp/zlink-dotnet-pub-twoslot-c/multi/report/perf_c_multi_linux_20260813_012815_dotnet-pub-twoslot-c.txt`
- .NET: `/tmp/zlink-dotnet-pub-twoslot-dotnet/multi/report/perf_dotnet_multi_linux_20260813_012849_dotnet-pub-twoslot-dotnet.txt`

## 채택한 receive wrapper pool 연결

`TopicMessage`가 새 private receive wrapper를 만들 때 기존 `Message` ThreadLocal
pool을 사용하도록 연결했다. pool에는 `Dispose()`로 native handle을 닫은 wrapper만
들어가므로, 호출자가 보관 중인 공개 `Message`를 다음 receive에 재사용하지 않는다.
이 변경으로 `pubsub_single_part_pool_reuses_only_released_wrapper` 계약 테스트가 통과했다.

multi perf는 client별 `TopicMessage`를 active phase 동안 재사용하므로 steady-state
throughput을 이 pool 변경의 효과로 해석하지 않는다. 새 envelope를 반복해서 만드는
일반 사용 경로의 managed wrapper 할당을 줄이는 구조 개선으로 채택한다.

## 채택한 DONT_WAIT native transition

poller가 ready socket을 반환한 뒤 수신 queue를 drain하는 `RecvFlags.DontWait` 경로는
blocking transport I/O를 기다리지 않는다. 이 경로에만 `SuppressGCTransition` native
import를 사용하고, 일반 blocking receive는 기존 import를 유지했다. `msg_size`와
`msg_data`는 이미 같은 최적화가 적용되어 있다.

100 clients, TCP, 3초 active duration, auto-HWM balanced, I/O thread 4, release Core
0.10.1 조건에서 C를 먼저 실행하고 .NET을 실행했다.

| Size | C throughput | .NET throughput | C 대비 |
|---:|---:|---:|---:|
| 64B | 1,626,870.3 msg/s | 1,076,473.0 msg/s | 66.16% |
| 256B | 1,677,318.7 msg/s | 1,171,615.3 msg/s | 69.85% |
| 1024B | 1,425,674.0 msg/s | 953,951.7 msg/s | 66.91% |
| 4096B | 575,030.3 msg/s | 494,540.3 msg/s | 86.00% |
| 65536B | 112,829.7 msg/s | 112,146.0 msg/s | 99.39% |
| 131072B | 52,464.7 msg/s | 62,087.0 msg/s | 118.34% |

산술평균은 `84.44%`다. 이전 확인값 `79.05%`보다 높지만 목표 `85%`에는 `0.56%p`
미달한다. `test_pubsub` 15건도 통과했다.

- C: `/tmp/zlink-dotnet-topic-pool-c/multi/report/perf_c_multi_linux_20260813_035201_dotnet-topic-pool-c.txt`
- .NET: `/tmp/zlink-dotnet-subscribe-dontwait-dotnet/multi/report/perf_dotnet_multi_linux_20260813_035508_dotnet-subscribe-dontwait.txt`

`DONT_WAIT` wrapper의 바깥 exception filter 제거도 확인했지만 추가 개선을 재현하지
못했고 multipart 오류 변환 범위를 바꿀 이유가 없었다. 이 후보는 채택하지 않았다.

`zlink_msg_size`와 `zlink_msg_data`를 한 native call로 합치려면 Core public C ABI를
변경하거나 별도 native shim을 package에 추가해야 한다. 전자는 이 작업의 제약에 맞지
않고 후자는 배포 구조를 넓히므로, 현재 .NET PUB/SUB 대상의 다음 작은 hot-path 후보로
채택하지 않았다.
