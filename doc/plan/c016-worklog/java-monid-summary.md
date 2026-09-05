# Java monitor connection identity test correction

STATUS: COMPLETE_WITH_BLOCKER

## 변경

- `bindings/java/src/test/java/systems/zlink/contract/MonitorConnectionIdentityContractTest.java`
  - inproc READY/DISCONNECTED 및 re-bind 재연결 case 활성화
  - TCP 서버 close 후 `READY == DISCONNECTED`를 검증하고, `CLOSED`는 이전 READY와 다른 새 attempt ID임을 검증
  - `CONNECT_DELAYED`가 관찰되면 `CONNECT_DELAYED == CLOSED`의 connection ID와 transport lane을 검증
  - inproc CLOSED case는 `@Disabled("spec gap: inproc peer close emits no CLOSED — D-B102")`로 사유 갱신

## 검증

- `compileTestJava`: PASS
- `MonitorConnectionIdentityContractTest`: 5/5 PASS (참고값)
- `MonitorPollingContractTest`: 5/5 PASS (참고값)
- `bindings/java/tests/run_tests.sh`: PASS — test, integrationTest, Netty, Kotlin, sample 7개 모두 통과 (참고값)
- `git diff --check`: PASS

## BLOCKERS

- core/build stale — 감독자 재빌드 후 재실행 필요
  - `core/build/lib/libzlink.so.0.17.0`: 2026-09-05 03:47:57 +0900
  - Core 수정 `1c69086a4a`: 2026-09-05 10:21:12 +0900
  - 위 runtime 테스트 결과는 stale 라이브러리에서 얻은 참고값이며, 재빌드된 Core로 재검증해야 한다.

## 참고

- 최초 선택 테스트 명령은 멀티프로젝트 `test` selector가 `--tests`를 하위 프로젝트에도 적용해 `perf-multi`의 “No tests found”로 종료됐다. 대상 루트 `:test` task로 바로잡아 각 클래스 5회를 별도로 모두 통과했다.
