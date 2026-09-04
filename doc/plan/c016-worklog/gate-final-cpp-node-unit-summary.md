# 최종 C++·Node framework unit gate 결과

committed `main`에서 실행했다. Core와 local package는 다시 빌드하지 않았고, 소스·test는 수정하지 않았다. 기존 worktree 변경은 보존했다.

## C++

`linux-ninja-c-e2e` build refresh는 `ninja: no work to do.`로 성공했다. 전체 CTest는 **68/69 pass, 1 fail**(370.71초)였다. CMake에 wiring된 package consumer 두 건(`test_cpp_framework_install_consumer`, `test_cpp_stream_connector_install_consumer`)은 전체 CTest에서 모두 통과했다. `test_cpp_framework_m6b_runtime`도 통과했으므로 이번 실행에서 line 1909 cross-clock flake는 발생하지 않았다.

| 실패 test | 최초 실패 assertion (`file:line`) | 분류 | 재실행·결정성 | prior evidence |
|---|---|---|---|---|
| `test_cpp_framework_common_e2e_inventory` | `FAIL: 278 required inventory conditions are open` — `framework/languages/cpp/e2e/verify_common_inventory.sh:161` | C — known pre-existing | 단독 재실행도 동일 278로 실패; deterministic | `doc/plan/c016-worklog/gate-v2-cpp-summary.md:26` |

분류 A(DONTWAIT/backpressure), B(terminal/error), D(environment/runner), E(binding-port dependency)는 없다. `m6b`의 알려진 cross-clock 후보는 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:1909`이며 prior evidence는 `doc/plan/c016-worklog/gate-v2-cpp-summary.md:25` 및 `doc/plan/c016-worklog/bucketB-cpp-m6b-late-summary.md:17-18`이다. 이번 full CTest에서는 해당 test가 pass했으므로 실패 재실행 대상이 아니었다.

실행 명령:

```bash
mkdir -p /dev/shm/zlink-tmp-cpp
TMPDIR=/dev/shm/zlink-tmp-cpp cmake --build framework/languages/cpp/build/linux-ninja-c-e2e -j4
TMPDIR=/dev/shm/zlink-tmp-cpp ctest --test-dir framework/languages/cpp/build/linux-ninja-c-e2e --output-on-failure -j2
TMPDIR=/dev/shm/zlink-tmp-cpp ctest --test-dir framework/languages/cpp/build/linux-ninja-c-e2e --output-on-failure -j1 -R '^test_cpp_framework_common_e2e_inventory$'
```

로그:

- `zlink-work/c016/logs/gate-final-unit-cpp-build.log`
- `zlink-work/c016/logs/gate-final-unit-cpp-ctest.log`
- `zlink-work/c016/logs/gate-final-unit-cpp-inventory-rerun.log`

## Node

`samples/ZoneWorld/dist`는 gate 전 확인 시 존재했다(`Client`, `Server`, `Shared`, `browser`). 따라서 과거의 dist-not-built 3건에 대한 sample build 및 test 재실행은 필요하지 않았다.

`npm run build`는 성공했다. `package.json`의 `npm test`는 `node scripts/run_node_runtime_gate.js`이며, build와 typecheck는 성공했으나 lint에서 중단됐다. 그러므로 full runtime test 실행 단계는 **0 tests reached**다. 독립 `npm run lint`도 동일 오류로 실패했고 한 번 재실행해 결정적으로 재현했다: **1 error, 0 warnings**.

| gate | 실패 assertion (`file:line`) | 분류 | 재실행·결정성 | prior evidence |
|---|---|---|---|---|
| `npm test` / `npm run lint` | `@typescript-eslint/strict-boolean-expressions`: nullable boolean condition — `framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts:137:12` | C — known pre-existing | 독립 lint 재실행도 동일 1 error; deterministic | `doc/plan/c016-worklog/gate-node-bootstrap-summary.md:51` |

분류 A(DONTWAIT/backpressure), B(terminal/error), D(environment/runner), E(binding-port dependency)는 없다. ZoneWorld dist-not-built의 prior environment evidence는 `doc/plan/c016-worklog/gate-node-bootstrap-summary.md:54-56`이나, 이번에는 dist가 존재하고 lint 이전에 별도 ZoneWorld 실패가 관찰되지 않았다.

모든 Node 명령은 `framework/languages/node`에서 `flock -w7200 /tmp/zlink-node-gate.lock` 안에 실행했고 `ZLINK_LIBRARY_PATH`를 unset했다.

```bash
flock -w7200 /tmp/zlink-node-gate.lock bash -lc 'unset ZLINK_LIBRARY_PATH; TMPDIR=/dev/shm/zlink-tmp-node npm run build'
flock -w7200 /tmp/zlink-node-gate.lock bash -lc 'unset ZLINK_LIBRARY_PATH; TMPDIR=/dev/shm/zlink-tmp-node npm test'
flock -w7200 /tmp/zlink-node-gate.lock bash -lc 'unset ZLINK_LIBRARY_PATH; TMPDIR=/dev/shm/zlink-tmp-node npm run lint'
flock -w7200 /tmp/zlink-node-gate.lock bash -lc 'unset ZLINK_LIBRARY_PATH; TMPDIR=/dev/shm/zlink-tmp-node npm run lint'  # failure rerun
```

로그:

- `zlink-work/c016/logs/gate-final-unit-node-build.log`
- `zlink-work/c016/logs/gate-final-unit-node-test.log`
- `zlink-work/c016/logs/gate-final-unit-node-lint.log`
- `zlink-work/c016/logs/gate-final-unit-node-lint-rerun.log`

## BLOCKERS

- C++ full CTest: known inventory gap 278 (`verify_common_inventory.sh:161`), deterministic.
- Node standard unit gate: known ESLint blocker (`spot-timer.ts:137:12`), deterministic; it prevents the runtime-test stage of `npm test` from running.
