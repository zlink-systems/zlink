# #4190 Node 보고서

## 결과

- #4191의 §10.2–§10.3에 맞춰 Node `request-backpressure`와 `send-saturation`을 논리 stream 8개로 실행한다. 모든 stream은 Node event loop 하나에서 진행한다.
- `request-serial`은 stream 1개, stream당 in-flight 1로 유지했다.
- runner trigger와 Node CLI/result metadata 필드 이름을 `submitConcurrency`로 바꿨다.
- Binding과 Framework smoke 셀 6개 모두 완료 수가 0보다 크고 source/server errors가 0이었다.
- Node에는 전용 submit OS thread가 없으므로 `/proc/<pid>/task` 목록은 해당하지 않는다.

## 판정 위치

- 제출 stream 수 결정 전: request-backpressure는 단일 loop, send-saturation은 `sendConcurrency` 수의 loop였다 (`bench-core.js` 변경 전 307, 386–439; `main.js` 변경 전 114).
- 제출 stream 수 결정 후: `bench-core.js:223–243`에서 trigger 값을 pattern driver에 전달하고, request-backpressure는 `:299–320`, send-saturation은 `:410–441`에서 stream별 loop를 만든다. `main.js:114–115`는 gRPC client/channel 하나를 공유한다.
- sequence 전/후: `bench-core.js:202–203`의 phase 단위 `nextSequence`를 유지한다. 단일 event loop의 동기 증가이므로 stream 간 값이 겹치지 않는다. reply 검증은 raw submission callback `:357–364`, 일반 request `:444–453`에서 발급한 sequence별로 수행한다.
- 집계 병합 전/후: Node는 여러 OS stream thread가 없어 thread별 집계 병합을 두지 않는다. stream loop들이 event loop에서 순차 실행되는 동안 `SourceMetrics.begin/complete/recordError` (`bench-core.js:40–102`)에 합산하고, 모든 loop 완료 뒤 `runActive`의 terminal snapshot 및 result 생성(`bench-core.js:246–268`)으로 phase 집계를 확정한다.

## 바꾼 것

- `framework/bench/grpc/node/client/bench-core.js`: backpressure stream별 loop/admission 상태, send trigger 이름, stream metadata.
- `framework/bench/grpc/node/client/main.js`: CLI와 metadata rename, gRPC channel/client 하나 공유.
- `framework/bench/grpc/node/shared/bench-http-application.js`: trigger schema와 validation rename.
- `framework/bench/grpc/node/run_local.sh`: `--submit-concurrency` 전달.
- `framework/bench/grpc/node/client/bench-core.test.js`: 두 패턴의 stream 수 및 stream별 admission을 검증.
- `framework/bench/grpc/runner_common.sh`: 지정된 두 줄의 `SUBMIT_CONCURRENCY`/`submitConcurrency` rename만 적용.
- 중첩 경로 `framework/bench/grpc/node/framework/bench/grpc/log/` 아래 실수로 커밋된 34개 run output 파일을 삭제했다. smoke 결과는 JOB 폴더에 보관했다.
- 리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 1건 발견. 고친 항목: gRPC client/channel을 하나로 공유해 8개 client 생성 경로를 제거. 넘긴 항목: 없음.

## 검증

- `npm test --prefix framework/bench/grpc/node`: 17/17 통과 (`node-tests.log`). 최초 실행은 누락된 local package 의존성 때문에 raw-wire 테스트만 실행 실패했으며, JOB 경로에서 Node binding/http-client package를 준비한 뒤 재실행해 통과했다.
- Node smoke: `run_local.sh`, `RUNS=1`, `DURATION_SECONDS=5`, `WARMUP_SECONDS=5`, `PAYLOAD_SIZES=1024`, implementations `zlink-node,zlink-framework-node`, patterns `request-backpressure,send-saturation,request-serial`. `/root/jobs/fx/cx/measure.lock` 아래 실행했다 (`node-smoke.log`, 결과 디렉터리 `node-smoke/run1/`).
- 결과 JSON의 `metadata.submitConcurrency`는 8. backpressure/send stream count는 8이고 serial stream count는 1이다. 세 pattern 모두 errors와 server_errors는 0이다.
- 완료 수: binding backpressure 153,832; send 541,211; serial 22,844. Framework backpressure 82,699; send 373,353; serial 23,104. runner의 settle 검증은 각 셀에서 통과했다.
- `node --check` (변경한 JS 세 파일), `bash -n` (Node runner와 공통 runner), `git diff --check`: 통과.
- smoke/package 준비 중 생성된 ignored build outputs는 worktree에서 제거했다. 재검증된 `node_modules`, `dist`, `build`, prebuild outputs는 남기지 않았다.

## 남은 문제

- 없음.

## 커밋

- `7eb35d05f1` (`bench(node): submit eight logical streams`), branch `bench/4190-s4190n`; push하지 않았다.
