| Core↔managed 경계 | Framework p50/p90/평균 (µs) | Binding p50/p90/평균 (µs) | 차이 p50/p90/평균 (µs, binding−Framework) |
|---|---:|---:|---:|
| target recvfrom → handler start (B1/T1) | 42.067 / 54.412 / 44.148 | 17.583 / 24.613 / 19.258 | −24.484 / −29.799 / −24.890 |
| reply call (B4/T4) → target sendto | 23.113 / 31.598 / 25.895 | 16.671 / 22.782 / 17.945 | −6.442 / −8.816 / −7.950 |
| source recvfrom → reply observed (D1/S1) | 68.175 / 86.132 / 70.976 | 34.182 / 42.989 / 35.765 | −33.993 / −43.143 / −35.211 |
| reply observed (D4/S4) → next source sendto | 31.889 / 41.386 / 34.048 | 16.180 / 21.290 / 17.108 | −15.709 / −20.096 / −16.940 |

Binding 실행의 Core syscall과 managed 표식 사이 전체 연속 구간은 다음과 같다. 각 행은 요청 순서로 연결된 완전한 19,643개 cycle을 사용했다.

| 연속 구간 | p50 (µs) | p90 (µs) | 평균 (µs) | 시작 thread → 끝 thread |
|---|---:|---:|---:|---|
| source sendto → target recvfrom | 28.713 | 35.712 | 36.420 | source ZLINKbg/IO/0 TID 3408112 → target ZLINKbg/IO/0 TID 3408027 |
| target recvfrom → T1 | 17.583 | 24.613 | 19.258 | target ZLINKbg/IO/0 TID 3408027 → .NET ThreadPool worker managed ID 7 |
| T1 → T4 | 0.521 | 1.653 | 0.800 | .NET ThreadPool worker managed ID 7 → 같은 handler worker |
| T4 → target sendto | 16.671 | 22.782 | 17.945 | .NET ThreadPool worker managed ID 7 → target ZLINKbg/IO/1 TID 3408028 |
| target sendto → source recvfrom | 28.633 | 35.876 | 29.964 | target ZLINKbg/IO/1 TID 3408028 → source ZLINKbg/IO/2 TID 3408114 |
| source recvfrom → S1 | 34.182 | 42.989 | 35.765 | source ZLINKbg/IO/2 TID 3408114 → .NET ThreadPool worker |
| S1 → S4 | 2.023 | 4.108 | 2.773 | .NET ThreadPool worker → 다음 제출을 수행한 .NET ThreadPool worker |
| S4 → 다음 source sendto | 16.180 | 21.290 | 17.108 | .NET ThreadPool worker → source ZLINKbg/IO/0 TID 3408112 |

source recvfrom 뒤 completion poller의 `Wait` 반환과 caller continuation은 같은 thread가 아니었다. completion은 managed thread ID 14의 `.NET ThreadPool worker`에서 drain됐고, S1은 각 요청마다 다른 ThreadPool worker에서 관측됐다. 19,643개 cycle 중 같은 thread는 0개, 다른 thread는 19,643개였다. source recvfrom syscall은 TID 3408114의 `ZLINKbg/IO/2`에서 실행됐다.

Framework 처리량은 5,360.067 req/s였고 Binding 처리량은 9,421.800 req/s였다. Framework 값은 `/root/jobs/fx/cx/v21/s033/dseg2/report.md`의 같은 1 KiB request-serial 실행에서 가져왔다. Framework p50/p90/평균은 각각 target recvfrom→B1 42.067/54.412/44.148 µs, B4→target sendto 23.113/31.598/25.895 µs, source recvfrom→D1 68.175/86.132/70.976 µs, D4→다음 sendto 31.889/41.386/34.048 µs다. 두 실행의 처리량은 각 전체 benchmark active 15초 기준이다.

Binding은 `zlink-dotnet`, request-serial, payload 1,024 bytes로 실행했다. 빌드는 `flock -s /root/jobs/fx/cx/measure.lock taskset -c 0-7 bash dotnet/build.sh`로 수행했다. 실행은 `flock -o /root/jobs/fx/cx/measure.lock`으로 잠금을 얻은 뒤 진행했고, perf는 두 benchmark PID에 `raw_syscalls:sys_enter`, `raw_syscalls:sys_exit`, `sched:sched_switch`, `sched:sched_wakeup`을 `-k mono`로 부착해 active 구간에서 3초 기록했다. 실제 기록 길이는 perf clock 기준 3.020초였으며, 측정 시 MemAvailable은 12 GiB 이상이었다.

perf는 5,806,673개 이벤트를 처리했고 2개 data chunk 유실과 3개 out-of-order event를 보고했다. `perf record` 로그에는 유실 chunk의 raw event 개수가 나오지 않아 개별 유실 event 수는 확인할 수 없다. syscall 표본 일부가 빠졌을 수 있다. perf 대상 PID는 3408100과 3408002였고 최종 trace 파일 PID와 일치한다. active perf 구간에서 연결된 완전 cycle은 19,643개다.

계측은 Stopwatch 기반 고정 ring buffer를 재사용했고 환경 변수 `ZLINK_DSEG_TRACE` gate를 유지했다. 코드 계측을 포함한 빌드는 오류 없이 끝났다. 이번 작업에서 수정한 source 파일은 다음과 같다.

- `framework/bench/grpc/dotnet/Client/DsegBenchTrace.cs`
- `framework/bench/grpc/dotnet/Client/Program.cs`
- `framework/bench/grpc/dotnet/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj`
- `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs`

측정과 분석을 위해 생성한 파일은 다음과 같다.

- `/root/jobs/fx/cx/v21/s033/dseg3/capture.py`
- `/root/jobs/fx/cx/v21/s033/dseg3/analyze.py`
- `/root/jobs/fx/cx/v21/s033/dseg3/report.md`

`bindings/`는 수정하지 않았고 commit도 만들지 않았다.
