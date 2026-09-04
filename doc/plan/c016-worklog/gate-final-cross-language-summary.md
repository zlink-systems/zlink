# Final cross-language E2E gate (plan §D)

실행일: 2026-09-05. `main`에서 수행했다. Core, local package, source 및 test는 다시 만들거나 수정하지 않았다. Node build만 요청에 따라 smoke 전에 한 번 실행했다.

## 결과

| runner | 결과 | 통과 수 | 시간 | 로그 |
| --- | --- | ---: | ---: | --- |
| C++ all-stage (clean-environment rerun) | PASS | 32/32 | 299s | `zlink-work/c016/logs/gate-final-cross-cpp.log` |
| Node smoke | PASS | 12/12 stage (세부 `ok` 19개) | build 2s, smoke 42s | `zlink-work/c016/logs/gate-final-cross-node.log` |
| Java `java-cross` | PASS | 4/4 | 11s | `zlink-work/c016/logs/gate-final-cross-java.log` |

Node log에는 `Node Redis opaque Location Store round trip` 및 `dotnet Redis opaque Location Store round trip`이 모두 `ok`로 남아 있어 Redis stage가 실제 실행됐음을 확인했다.

C++ all-stage의 clean-environment rerun은 messaging 12개, spot-route 7개, relocation 1개, User-Spot Join 12개를 모두 통과했다.

## 실행 명령

```bash
# Node smoke 전에 한 번만 실행
cd framework/languages/node
TMPDIR=/dev/shm/zlink-tmp-node \
  flock -w7200 /tmp/zlink-node-gate.lock npm run build

# C++ all-stage: Java peer도 포함하므로 Java가 directory를 file로 해석하지 않게 unset
export TMPDIR=/dev/shm/zlink-tmp-dotnet
export ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e
unset ZLINK_LIBRARY_PATH
export UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash=$(sha256sum .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg | awk '{print $1}')
export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
flock -w7200 /tmp/zlink-dotnet-gate.lock \
  flock -w7200 /tmp/zlink-node-gate.lock \
  flock -w7200 /tmp/zlink-jvm-gate.lock \
  framework/languages/cpp/cross-language/run_cross_language_smoke.sh

# Node smoke
export TMPDIR=/dev/shm/zlink-tmp-node
unset ZLINK_LIBRARY_PATH
flock -w7200 /tmp/zlink-node-gate.lock \
  flock -w7200 /tmp/zlink-dotnet-gate.lock \
  framework/languages/node/cross-language/run_cross_language_smoke.sh

# Java 4-way selector
export TMPDIR=/dev/shm/zlink-tmp-java
unset ZLINK_LIBRARY_PATH
ZLINK_CPP_CROSS_LANGUAGE_STAGE=java-cross \
  flock -w7200 /tmp/zlink-jvm-gate.lock \
  flock -w7200 /tmp/zlink-node-gate.lock \
  flock -w7200 /tmp/zlink-dotnet-gate.lock \
  framework/languages/cpp/cross-language/run_cross_language_smoke.sh
```

## 실패 및 분류

| bucket | 결과 | 첫 실패 / 근거 | 재실행 |
| --- | --- | --- | --- |
| A: DONTWAIT/backpressure | 없음 | BACKPRESSURED/EAGAIN/native errno 없음 | 해당 없음 |
| B: terminal/error classification | 없음 | 최종 green 실행에서 terminal error 없음 | 해당 없음 |
| C: known pre-existing | 없음 | 이 gate에서 이전 알려진 실패가 재현되지 않음 | 해당 없음 |
| D: environment/runner | 해결 | 첫 C++ all 실행은 Java client의 reply 대기로 실패: `framework/languages/cpp/cross-language/run_cross_language_smoke.sh:708` (`wait_for_line`, `java-to-cpp-spot`). 이 실행은 Java가 file path로 취급하는 directory형 `ZLINK_LIBRARY_PATH`를 상속했다. | `ZLINK_LIBRARY_PATH` unset 뒤 spot-route 7/7 및 all-stage 32/32 PASS |
| E: binding-port dependency | 없음 | A의 submit/backpressure 증거 또는 binding-port 의존성 없음 | 해당 없음 |

첫 C++ all-stage 시도는 lock 대기 포함 807s, exit 1이었다. 실패한 spot-route selector를 한 번 재실행해 22s, 7/7 PASS를 기록했고, 이어 clean-environment all-stage rerun을 수행했다. 따라서 결정론적 product failure가 아니라 환경 전달 문제로 분류한다.

## BLOCKERS

없음. 최종 C++ all-stage, Node smoke (Redis 포함), Java cross 모두 통과했다.
