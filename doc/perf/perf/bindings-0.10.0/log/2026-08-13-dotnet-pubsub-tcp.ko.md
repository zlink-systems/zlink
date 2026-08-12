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
