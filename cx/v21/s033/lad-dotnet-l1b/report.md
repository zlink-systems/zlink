# .NET ladder L1 측정

## 호출 형태

Binding/L0/L1은 공통 raw client API를 사용한다. L0는 고정 handler 호출만 복원하고 L1은 receive 전 permit 획득, handler 첫 명령 전 permit 반환을 더한다. 네 경로와 per-message 비용은 [call-shape.md](call-shape.md)에 적었다.

## L0 fresh

| 셀 | 반복 | L0 median msg/s | Binding median msg/s | L0/Binding | G5 spread L0/B · B | 판정 | Raw |
|---|---:|---:|---:|---:|---:|---|---|
| send-saturation 1 KiB | 3 | 431,231.6 | 433,456.6 | 0.9949 | 4.9% · 2.1% | 통과 | `raw/session/{binding,L0}/run-{1..3}/run1/zlink-dotnet-send-saturation-1024/results.json` |
| send-saturation 4 KiB | 5 | 176,210.4 | 174,710.8 | 1.0086 | 4.8% · 10.1% | 판정 불가 | `raw/session/{binding,L0}/run-{1..3}/run1/zlink-dotnet-send-saturation-4096/results.json`; `raw/session/{binding,L0}/extra-send-saturation-4096/run-{4,5}/run1/zlink-dotnet-send-saturation-4096/results.json` |
| request-serial 1 KiB | 5 | 8,157.2 | 6,953.2 | 1.1732 | 22.1% · 21.9% | 판정 불가 | `raw/session/{binding,L0}/run-{1..3}/run1/zlink-dotnet-request-serial-1024/results.json`; `raw/session/{binding,L0}/extra-request-serial-1024/run-{4,5}/run1/zlink-dotnet-request-serial-1024/results.json` |
| request-serial 4 KiB | 5 | 7,382.2 | 7,372.0 | 1.0014 | 54.5% · 24.9% | 판정 불가 | `raw/session/{binding,L0}/run-{1..3}/run1/zlink-dotnet-request-serial-4096/results.json`; `raw/session/{binding,L0}/extra-request-serial-4096/run-{4,5}/run1/zlink-dotnet-request-serial-4096/results.json` |
| request-backpressure 1 KiB | 5 | 108,876.0 | 130,150.0 | 0.8365 | 95.6% · 54.1% | 판정 불가 | `raw/session/{binding,L0}/run-{1..3}/run1/zlink-dotnet-request-backpressure-1024/results.json`; `raw/session/{binding,L0}/extra-request-backpressure-1024/run-{4,5}/run1/zlink-dotnet-request-backpressure-1024/results.json` |
| request-backpressure 4 KiB | 5 | 90,616.4 | 84,622.8 | 1.0708 | 10.7% · 10.8% | 판정 불가 | `raw/session/{binding,L0}/run-{1..3}/run1/zlink-dotnet-request-backpressure-4096/results.json`; `raw/session/{binding,L0}/extra-request-backpressure-4096/run-{4,5}/run1/zlink-dotnet-request-backpressure-4096/results.json` |

## L1 및 단계 손실

| 셀 | 반복 | L1 median msg/s | Binding median msg/s | L1/Binding | L0/Binding | 단계 손실 | G5 spread L1/B · B | 판정 | Raw |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation 1 KiB | 3 | 421,828.6 | 433,456.6 | 0.9732 | 0.9949 | 2.2% | 3.8% · 2.1% | 통과 | `raw/session/{binding,L0,L1}/run-{1..3}/run1/zlink-dotnet-send-saturation-1024/results.json` |
| send-saturation 4 KiB | 5 | 179,198.0 | 174,710.8 | 1.0257 | 1.0086 | -1.7% | 10.6% · 10.1% | 판정 불가 | `raw/session/{binding,L0,L1}/run-{1..3}/run1/zlink-dotnet-send-saturation-4096/results.json`; `raw/session/{binding,L0,L1}/extra-send-saturation-4096/run-{4,5}/run1/zlink-dotnet-send-saturation-4096/results.json` |
| request-serial 1 KiB | 5 | 8,490.2 | 6,953.2 | 1.2210 | 1.1732 | -4.1% | 40.3% · 21.9% | 판정 불가 | `raw/session/{binding,L0,L1}/run-{1..3}/run1/zlink-dotnet-request-serial-1024/results.json`; `raw/session/{binding,L0,L1}/extra-request-serial-1024/run-{4,5}/run1/zlink-dotnet-request-serial-1024/results.json` |
| request-serial 4 KiB | 5 | 6,069.0 | 7,372.0 | 0.8233 | 1.0014 | 17.8% | 38.6% · 24.9% | 판정 불가 | `raw/session/{binding,L0,L1}/run-{1..3}/run1/zlink-dotnet-request-serial-4096/results.json`; `raw/session/{binding,L0,L1}/extra-request-serial-4096/run-{4,5}/run1/zlink-dotnet-request-serial-4096/results.json` |
| request-backpressure 1 KiB | 5 | 123,362.4 | 130,150.0 | 0.9478 | 0.8365 | -13.3% | 43.7% · 54.1% | 판정 불가 | `raw/session/{binding,L0,L1}/run-{1..3}/run1/zlink-dotnet-request-backpressure-1024/results.json`; `raw/session/{binding,L0,L1}/extra-request-backpressure-1024/run-{4,5}/run1/zlink-dotnet-request-backpressure-1024/results.json` |
| request-backpressure 4 KiB | 5 | 90,486.6 | 84,622.8 | 1.0693 | 1.0708 | 0.1% | 13.0% · 10.8% | 판정 불가 | `raw/session/{binding,L0,L1}/run-{1..3}/run1/zlink-dotnet-request-backpressure-4096/results.json`; `raw/session/{binding,L0,L1}/extra-request-backpressure-4096/run-{4,5}/run1/zlink-dotnet-request-backpressure-4096/results.json` |

## request-serial G5 판정 불가 근거

다음은 각 5회 raw `results.json`의 generatedUtc, source/target CPU와 시작 스탬프다. `lock-windows`의 `*-before.ps.txt`/`*-after.ps.txt` 및 세션 before/after process snapshot에서 잠금 중 실행 중인 프로세스를 확인한다.

| 실행 중 다른 프로세스 | 최대 CPU | lock-window 근거 |
|---|---:|---|
| `bfs` 작업 트리 검색 | 212.0% | `raw/lock-windows/binding-request-serial-4096-run4-before.ps.txt` |
| `zstd` 압축 | 178.0% | `raw/lock-windows/L1-request-serial-4096-run5-before.ps.txt` |
| `du` 디스크 사용량 검색 | 81.5% | `raw/lock-windows/binding-request-serial-1024-run4-before.ps.txt` |
| `tar` 압축, GitHub `ssh`, Claude/Codex 및 guard 프로세스 | 최대 29.1%, 4.1%, 2.9%, 2.2% | `raw/lock-windows/L1-request-serial-4096-run4-before.ps.txt`, `raw/lock-windows/L0-request-serial-1024-run4-before.ps.txt`, `raw/lock-windows/L0-request-serial-1024-run3-before.ps.txt`, `raw/lock-windows/L0-request-serial-1024-run1-before.ps.txt` |

다섯 반복 뒤에도 request-serial G5가 모두 10%를 넘었다. 잠금은 측정 간 배타적이었지만 다른 CPU 작업은 계속됐으므로 두 셀은 판정 불가다. 프로세스 수치와 개별 run CPU는 위 표의 결과 파일 및 process snapshot을 근거로 했다.

| 변형/셀 | run | 시작 UTC | source CPU % | target CPU % | raw |
|---|---:|---|---:|---:|---|
| binding 1 KiB | 1 | 2026-11-13T14:47:31Z | 20.4 | 5.3 | `raw/session/binding/run-1/run1/zlink-dotnet-request-serial-1024/results.json` |
| binding 1 KiB | 2 | 2026-11-13T14:59:11Z | 20.0 | 4.9 | `raw/session/binding/run-2/run1/zlink-dotnet-request-serial-1024/results.json` |
| binding 1 KiB | 3 | 2026-11-13T15:10:50Z | 23.8 | 5.5 | `raw/session/binding/run-3/run1/zlink-dotnet-request-serial-1024/results.json` |
| binding 1 KiB | 4 | 2026-11-13T15:18:34Z | 19.8 | 5.3 | `raw/session/binding/extra-request-serial-1024/run-4/run1/zlink-dotnet-request-serial-1024/results.json` |
| binding 1 KiB | 5 | 2026-11-13T15:19:13Z | 23.6 | 5.1 | `raw/session/binding/extra-request-serial-1024/run-5/run1/zlink-dotnet-request-serial-1024/results.json` |
| L0 1 KiB | 1 | 2026-11-13T14:48:10Z | 23.2 | 5.7 | `raw/session/L0/run-1/run1/zlink-dotnet-request-serial-1024/results.json` |
| L0 1 KiB | 2 | 2026-11-13T14:59:49Z | 23.8 | 5.4 | `raw/session/L0/run-2/run1/zlink-dotnet-request-serial-1024/results.json` |
| L0 1 KiB | 3 | 2026-11-13T15:11:29Z | 24.1 | 5.3 | `raw/session/L0/run-3/run1/zlink-dotnet-request-serial-1024/results.json` |
| L0 1 KiB | 4 | 2026-11-13T15:19:52Z | 22.7 | 5.8 | `raw/session/L0/extra-request-serial-1024/run-4/run1/zlink-dotnet-request-serial-1024/results.json` |
| L0 1 KiB | 5 | 2026-11-13T15:20:30Z | 22.4 | 5.3 | `raw/session/L0/extra-request-serial-1024/run-5/run1/zlink-dotnet-request-serial-1024/results.json` |
| L1 1 KiB | 1 | 2026-11-13T14:48:49Z | 23.9 | 5.3 | `raw/session/L1/run-1/run1/zlink-dotnet-request-serial-1024/results.json` |
| L1 1 KiB | 2 | 2026-11-13T15:00:28Z | 23.8 | 5.6 | `raw/session/L1/run-2/run1/zlink-dotnet-request-serial-1024/results.json` |
| L1 1 KiB | 3 | 2026-11-13T15:12:08Z | 25.0 | 6.0 | `raw/session/L1/run-3/run1/zlink-dotnet-request-serial-1024/results.json` |
| L1 1 KiB | 4 | 2026-11-13T15:21:09Z | 26.1 | 6.2 | `raw/session/L1/extra-request-serial-1024/run-4/run1/zlink-dotnet-request-serial-1024/results.json` |
| L1 1 KiB | 5 | 2026-11-13T15:21:47Z | 20.8 | 5.1 | `raw/session/L1/extra-request-serial-1024/run-5/run1/zlink-dotnet-request-serial-1024/results.json` |
| binding 4 KiB | 1 | 2026-11-13T14:49:27Z | 23.0 | 5.7 | `raw/session/binding/run-1/run1/zlink-dotnet-request-serial-4096/results.json` |
| binding 4 KiB | 2 | 2026-11-13T15:01:07Z | 23.5 | 5.8 | `raw/session/binding/run-2/run1/zlink-dotnet-request-serial-4096/results.json` |
| binding 4 KiB | 3 | 2026-11-13T15:12:46Z | 22.5 | 5.9 | `raw/session/binding/run-3/run1/zlink-dotnet-request-serial-4096/results.json` |
| binding 4 KiB | 4 | 2026-11-13T15:22:26Z | 23.6 | 5.2 | `raw/session/binding/extra-request-serial-4096/run-4/run1/zlink-dotnet-request-serial-4096/results.json` |
| binding 4 KiB | 5 | 2026-11-13T15:23:05Z | 19.8 | 4.7 | `raw/session/binding/extra-request-serial-4096/run-5/run1/zlink-dotnet-request-serial-4096/results.json` |
| L0 4 KiB | 1 | 2026-11-13T14:50:06Z | 24.3 | 5.7 | `raw/session/L0/run-1/run1/zlink-dotnet-request-serial-4096/results.json` |
| L0 4 KiB | 2 | 2026-11-13T15:01:45Z | 21.9 | 5.4 | `raw/session/L0/run-2/run1/zlink-dotnet-request-serial-4096/results.json` |
| L0 4 KiB | 3 | 2026-11-13T15:13:24Z | 22.1 | 5.3 | `raw/session/L0/run-3/run1/zlink-dotnet-request-serial-4096/results.json` |
| L0 4 KiB | 4 | 2026-11-13T15:23:43Z | 23.2 | 5.1 | `raw/session/L0/extra-request-serial-4096/run-4/run1/zlink-dotnet-request-serial-4096/results.json` |
| L0 4 KiB | 5 | 2026-11-13T15:24:22Z | 24.6 | 5.8 | `raw/session/L0/extra-request-serial-4096/run-5/run1/zlink-dotnet-request-serial-4096/results.json` |
| L1 4 KiB | 1 | 2026-11-13T14:50:44Z | 24.6 | 5.8 | `raw/session/L1/run-1/run1/zlink-dotnet-request-serial-4096/results.json` |
| L1 4 KiB | 2 | 2026-11-13T15:02:24Z | 21.9 | 5.2 | `raw/session/L1/run-2/run1/zlink-dotnet-request-serial-4096/results.json` |
| L1 4 KiB | 3 | 2026-11-13T15:14:03Z | 20.8 | 5.2 | `raw/session/L1/run-3/run1/zlink-dotnet-request-serial-4096/results.json` |
| L1 4 KiB | 4 | 2026-11-13T15:25:01Z | 18.7 | 5.2 | `raw/session/L1/extra-request-serial-4096/run-4/run1/zlink-dotnet-request-serial-4096/results.json` |
| L1 4 KiB | 5 | 2026-11-13T15:25:40Z | 18.6 | 4.9 | `raw/session/L1/extra-request-serial-4096/run-5/run1/zlink-dotnet-request-serial-4096/results.json` |

## 측정 기록

- Build: `b6f58168a6` Release; `progress.md`에 각 측정 전 commit/dirty/base, DLL 경로/hash와 receive/reply 대조 기록.
- Lock window: `raw/lock-windows/session-before.txt`, `session-after.txt`; 모든 raw 결과는 보존.
- Step loss: `1 − (L1/Binding)/(L0/Binding)`; 통과 한도 7%.

## 7% 초과 셀 후속

| 셀 | L0/Binding | L1/Binding | 단계 손실 | 해석 |
|---|---:|---:|---:|---|
| request-serial 4 KiB | 1.0014 | 0.8233 | 17.8% | G5 spread 38.6%로 판정 불가; 프로파일 없이는 permit 비용으로 귀속하지 않음 (`summary-data.json`, 각 5개 raw 결과) |


| 프로파일 | 결과 | Raw |
|---|---|---|
| L0 request-serial 4 KiB, 1회 | `.NET TP Worker`의 kernel `__schedule`/`sched_yield` 12.69%, `libcoreclr.so` offset `0x2fc945` 6.62%; L1 짝이 없어 단계 비용으로 귀속하지 않음 | `raw/profiles/request-serial-4096/L0/run1/zlink-dotnet-request-serial-4096/capture/perf.data`, `capture/profile.txt` |

첫 L0 프로파일은 curl wrapper의 미설정 PID 환경변수로 target stats를 만들지 못했다. perf.data는 보존했지만 유효한 pair가 아니다 (`raw/profiles/request-serial-4096/L0/driver.log`, `results.json`). wrapper를 제거한 paired 재시도는 `raw/profiles/request-serial-4096-attempt2/`로 별도 보존하며, Node/`aa-noise`/Java 세션 뒤의 lock 대기열에 있다. profile 차등 비용은 현재 미확정이다. 코드상 후보는 acquire/lease [`ZLinkApplicationJobQueue.cs:208-223`](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:208), release/atomic [`ZLinkApplicationJobQueue.cs:520-534`](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:520) 및 [`ZLinkApplicationJobQueue.cs:1210-1211`](/root/worktree/zlink-v21-s033/lad-dotnet/framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:1210)다. 동등한 직렬화 비용을 binding 기준에 넣는 방안은 receive 직전 acquire, handler 첫 명령 전 반환을 binding 소유 계층에 두는 것이며, 구현은 하지 않는다.
