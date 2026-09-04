# 최종 .NET 공통 샘플 gate 결과 (round 2)

committed `main`에서 Core와 local package를 다시 빌드하지 않고 실행했다. 기존 worktree 변경은
보존했으며, 이 문서와 `zlink-work/c016/logs/`의 실행 로그만 추가했다.

## 개별 실행

| Sample | Result | Exit | Duration | Final marker |
|---|---|---:|---:|---|
| TicTacToe | pass | 0 | 28.624s | `tictactoe-placement=completed` |
| Bingo | pass | 0 | 27.750s | `bingo-placement=completed` |
| SupportChat | pass | 0 | 20.519s | `supportchat-placement=completed` |
| ShoppingMall | pass | 0 | 17.224s | `shoppingmall-placement=completed` |
| DeliveryDispatch | pass | 0 | 18.061s | `deliverydispatch-placement=completed` |
| GameQuest | pass | 0 | 21.930s | `gamequest-placement=completed` |
| ZoneWorld | pass | 0 | 239.038s | `zoneworld=completed` |

Individual logs:

- `zlink-work/c016/logs/gate-final-dotnet-2-TicTacToe.log`
- `zlink-work/c016/logs/gate-final-dotnet-2-Bingo.log`
- `zlink-work/c016/logs/gate-final-dotnet-2-SupportChat.log`
- `zlink-work/c016/logs/gate-final-dotnet-2-ShoppingMall.log`
- `zlink-work/c016/logs/gate-final-dotnet-2-DeliveryDispatch.log`
- `zlink-work/c016/logs/gate-final-dotnet-2-GameQuest.log`
- `zlink-work/c016/logs/gate-final-dotnet-2-ZoneWorld.log`

모든 개별 실행이 통과했으므로 failure retry는 발생하지 않았다.

## Aggregate runner

`run_samples.sh`를 인자 없이 한 번 실행했다. 7개 모두의 완료 marker를 순서대로 출력했고,
최종 exit는 **0**, duration은 **300.862s**였다. 마지막 marker는 `zoneworld=completed`다.
로그: `zlink-work/c016/logs/gate-final-dotnet-2-aggregate.log`.

## Failure classification

이번 실행에는 failing assertion, error terminal, 또는 runner failure가 없었다. 따라서 아래
bucket에 배정할 failure도 없다.

| Bucket | Result |
|---|---|
| A — DONTWAIT/backpressure | None; no failed run or backpressure assertion. |
| B — terminal/error classification | None; no failed run or terminal-error assertion. |
| C — known pre-existing | None. D-084의 prior evidence는 `doc/plan/c016-worklog/decisions.ko.md:823-841`이나, 이번 ZoneWorld 실행의 exact completion assertion/marker는 `zlink-work/c016/logs/gate-final-dotnet-2-ZoneWorld.log:147`의 `zoneworld=completed`이고 exit 0이다. 따라서 count나 증상 유사성으로 C를 배정하지 않았다. |
| D — environment/runner | None; Redis scoped containers, all individual runners, and aggregate runner completed. |
| E — binding-port dependency | None observed. |

## Commands

Every invocation used this environment and lock (without `--artifacts-path` or `ulimit -v`):

```bash
export TMPDIR=/dev/shm/zlink-tmp-dotnet ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash=$(sha256sum .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg | awk '{print $1}')
export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
flock -w7200 /tmp/zlink-dotnet-gate.lock bash framework/languages/dotnet/samples/run_samples.sh <Sample>
```

Executed `<Sample>` values: `TicTacToe`, `Bingo`, `SupportChat`, `ShoppingMall`,
`DeliveryDispatch`, `GameQuest`, and `ZoneWorld`. The aggregate invocation used the same command
without `<Sample>`.

## BLOCKERS

None.
