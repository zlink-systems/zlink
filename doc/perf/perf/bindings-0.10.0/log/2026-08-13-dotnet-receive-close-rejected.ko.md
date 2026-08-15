# .NET receive-only message close 후보 측정

## 후보

`TopicMessage`가 내부에서 교대 재사용하는 private receive wrapper에 한해
`zlink_msg_close`의 GC transition을 생략했다. 일반 `Message.Close`는 기존 import를
유지했다.

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 .NET, 병렬 실행 없음
- 공통 조건: `MULTI_PUBSUB`, tcp, clients `100`, duration `2초`, runs `1`
- C report: `/tmp/zlink-dotnet-recv-close-c/multi/report/perf_c_multi_linux_20260813_052330_dotnet-recv-close-c.txt`
- .NET report: `/tmp/zlink-dotnet-recv-close-dotnet/multi/report/perf_dotnet_multi_linux_20260813_052349_dotnet-recv-close-dotnet.txt`

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 1,555,213 msg/s | 1,001,668 msg/s | 64.41% |
| 256B | 1,629,756 msg/s | 1,098,795 msg/s | 67.42% |
| 1024B | 1,485,242 msg/s | 953,119 msg/s | 64.17% |
| 4096B | 589,309 msg/s | 451,835 msg/s | 76.67% |
| 65536B | 112,594 msg/s | 112,203 msg/s | 99.65% |
| 131072B | 58,787 msg/s | 58,247 msg/s | 99.08% |

산술평균은 `78.57%`다. 목표를 넘지 못했고 기존 경로 대비 명확한 개선도 없다.
native close의 실행 범위를 별도 import 가정으로 나누는 복잡성도 추가되므로 후보를
원복했다. 두 report는 모두 `status: complete`, result line `30/30`이다.
