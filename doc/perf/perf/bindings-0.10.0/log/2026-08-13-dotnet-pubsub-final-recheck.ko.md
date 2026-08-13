# .NET Pub/Sub 최종 재측정

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 .NET, 병렬 실행 없음
- 공통 조건: `MULTI_PUBSUB`, tcp, clients `100`, duration `3초`, runs `1`,
  I/O threads `4/4`, balanced auto-HWM, send/receive timeout `200ms`,
  connect-ready timeout `10000ms`
- C report: `/tmp/zlink-dotnet-pubsub-final-c/multi/report/perf_c_multi_linux_20260813_051456_dotnet-pubsub-final-c.txt`
- .NET report: `/tmp/zlink-dotnet-pubsub-final-dotnet/multi/report/perf_dotnet_multi_linux_20260813_051519_dotnet-pubsub-final-dotnet.txt`

| Size | C throughput | .NET throughput | .NET / C |
|---:|---:|---:|---:|
| 64B | 1,710,563 msg/s | 979,464 msg/s | 57.26% |
| 256B | 1,591,512 msg/s | 1,043,519 msg/s | 65.57% |
| 1024B | 1,492,021 msg/s | 987,484 msg/s | 66.18% |
| 4096B | 590,642 msg/s | 486,686 msg/s | 82.40% |
| 65536B | 114,293 msg/s | 107,363 msg/s | 93.94% |
| 131072B | 54,290 msg/s | 54,122 msg/s | 99.69% |

산술평균은 `77.51%`로 목표 `85%`에 미달한다. 두 report는 모두
`status: complete`, result line `30/30`이다.

## 병목 분리와 판정

64B 진단에서 .NET publisher는 2초 동안 `249,202`개 publish를 성공했다. fan-out
100 기준 공급 가능량은 초당 약 `12.46M delivery`이며 실제 aggregate receive는
초당 `1.03M delivery`였다. 따라서 publisher operation builder가 아니라 subscriber의
per-message native/managed 경계가 제한한다.

이미 적용된 caller-provided `TopicMessage`, private receive wrapper 재사용, lazy topic
decode, `DONT_WAIT` receive의 `SuppressGCTransition`, `msg_size`와 `msg_data` accessor의
`SuppressGCTransition`을 확인했다. 추가로 disposed-state 중복 검사 제거와 receive-only
`msg_close` transition 생략 후보를 각각 측정했지만 목표를 넘지 못했다. public interface와
Core C API를 유지하면서 적용할 추가 후보가 없어 이 행을 보류한다.
