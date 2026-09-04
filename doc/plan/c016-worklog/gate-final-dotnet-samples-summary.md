# 최종 .NET 공통 샘플 gate 결과

committed `main`에서 Core 및 local package를 다시 빌드하지 않고 실행했다. 개별 실행은
실패가 다음 샘플을 막지 않도록 한 샘플씩 수행했다. 기존 worktree 변경은 보존했고, 이 기록과
`zlink-work/c016/logs/` 외에는 변경하지 않았다.

## 개별 실행

| Sample | Result | Exit | Duration | Final marker |
|---|---|---:|---:|---|
| TicTacToe | fail (2/2 deterministic) | 134 | 31.895s / 14.913s | — |
| Bingo | pass | 0 | 36.147s | `bingo-placement=completed` |
| SupportChat | pass | 0 | 23.125s | `supportchat-placement=completed` |
| ShoppingMall | pass | 0 | 20.416s | `shoppingmall-placement=completed` |
| DeliveryDispatch | pass | 0 | 17.907s | `deliverydispatch-placement=completed` |
| GameQuest | pass | 0 | 20.159s | `gamequest-placement=completed` |
| ZoneWorld | pass | 0 | 248.891s | `zoneworld=completed` |

Individual logs: `zlink-work/c016/logs/gate-final-dotnet-<sample>.log`.

GameQuest log line 35 shows the intentionally killed owner server while exercising owner-loss;
the runner subsequently emitted its completion marker and exit 0, so it is not a gate failure.

## Aggregate runner

`run_samples.sh` was run once after all individual samples. It exited **134** in **20.099s** at
the first sample, TicTacToe; because the runner has `set -e`, it did not proceed to the other six.
It produced no completed marker. Log:
`zlink-work/c016/logs/gate-final-dotnet-samples-aggregate.log`.

## Failure classification

| Bucket | Finding and exact evidence | Verdict |
|---|---|---|
| A — DONTWAIT/backpressure | None. The three failing runs contain no `BACKPRESSURED`, `EAGAIN`, or DONTWAIT assertion. | No A failure. |
| B — terminal/error | TicTacToe’s first individual run reaches `JoinSpot` and logs `sent ... packet=JoinGameNotify`, but client then fails: `System.TimeoutException: Timed out waiting for 'JoinGameNotify' stream message.` Exact assertion/wait source: `framework/languages/dotnet/src/Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamReceivedMessages.cs:71`; sample call: `framework/languages/dotnet/samples/TicTacToe/Client/TicTacToeClientScenario.cs:240`. It is identical in both individual attempts and the aggregate run (aggregate evidence: `/dev/shm/zlink-tmp-dotnet/tmp.5n5J0qypjB/logs/client.log:23-27`). | B, deterministic terminal timeout. |
| C — known pre-existing | Not assigned. Related prior observation is `doc/plan/c016-worklog/decisions.ko.md:570-571` (non-deterministic TicTacToe bound-session delivery stall), but it does **not** state this exact `JoinGameNotify` timeout assertion. The worklog instead says a prior JoinGameNotify stall was not reproduced at `decisions.ko.md:783-786`; count or symptom similarity is insufficient for C. D-084 is ZoneWorld-specific (`decisions.ko.md:823-841`) and ZoneWorld passed here. | No verified C failure. |
| D — environment/runner | None. Redis scoped containers started for every Redis-dependent sample, builds completed, and six independent samples completed. | No D failure. |
| E — binding-port dependency | None observed. | No E failure. |

## Commands

Every invocation used this environment and lock (without `--artifacts-path` or `ulimit -v`):

```bash
export TMPDIR=/dev/shm/zlink-tmp-dotnet ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash=$(sha256sum .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg | awk '{print $1}')
export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
flock -w7200 /tmp/zlink-dotnet-gate.lock bash framework/languages/dotnet/samples/run_samples.sh <Sample>
```

Executed `<Sample>` values: `TicTacToe` (then one retry), `Bingo`, `SupportChat`,
`ShoppingMall`, `DeliveryDispatch`, `GameQuest`, and `ZoneWorld`. The aggregate invocation was
the same command without `<Sample>`.

## BLOCKERS

- TicTacToe blocks the aggregate gate: reproducible `JoinGameNotify` delivery timeout after
  server-side send. It needs framework/sample delivery investigation; this execution did not
  modify sources or tests.
