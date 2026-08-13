# .NET TopicMessage 중복 상태 검사 후보 측정

## 후보

caller-provided `TopicMessage` SUB receive에서 writable topic buffer가 이미
disposed 상태를 검사한 뒤 private message candidate와 single-part adopt가 다시
수행하는 `EnsureOpen()`을 제거했다.

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 .NET, 병렬 실행 없음
- 공통 조건: `MULTI_PUBSUB`, tcp, clients `100`, duration `2초`, runs `1`,
  I/O threads `4/4`, balanced auto-HWM, send/receive timeout `200ms`,
  connect-ready timeout `10000ms`
- C report: `/tmp/zlink-dotnet-topic-state-c/multi/report/perf_c_multi_linux_20260813_051757_dotnet-topic-state-c.txt`
- .NET report: `/tmp/zlink-dotnet-topic-state-dotnet/multi/report/perf_dotnet_multi_linux_20260813_051815_dotnet-topic-state-dotnet.txt`

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 1,432,606 msg/s | 978,772 msg/s | 68.32% |
| 256B | 1,632,279 msg/s | 1,065,313 msg/s | 65.27% |
| 1024B | 1,455,277 msg/s | 1,004,921 msg/s | 69.05% |
| 4096B | 598,362 msg/s | 456,447 msg/s | 76.28% |
| 65536B | 117,834 msg/s | 107,647 msg/s | 91.36% |
| 131072B | 51,741 msg/s | 58,579 msg/s | 113.22% |

산술평균은 `80.58%`다. 목표 `85%`에 미달하고, 일반 internal populate 경로의
disposed 상태 검사를 약화시키므로 후보는 원복했다. 두 report는 모두
`status: complete`, result line `30/30`이다.
