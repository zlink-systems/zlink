# DeliveryDispatch offer deadline parity 작업 결과

## C++

- `framework/languages/cpp/samples/DeliveryDispatch/Server/Dispatch/main.cpp:205-207`: 최초 배차에서 `Assigned` 상태 publish가 완료된 뒤 offer row와 900 ms deadline을 만들고 courier에게 전송한다.
- `framework/languages/cpp/samples/DeliveryDispatch/Server/Dispatch/main.cpp:243-246`: 재배차도 `Reassigned` 상태 publish 완료 뒤 offer row/deadline 생성과 courier 전송을 연속해서 수행한다.
- 별도 sample-level unit test는 추가하지 않았다. C++ DeliveryDispatch 샘플에는 test 프로젝트나 test target이 없고 실행형 샘플 runner만 있다.
- 샘플: `ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e framework/languages/cpp/samples/run_samples.sh DeliveryDispatch`
  - 성공 1: exit 0, `deliverydispatch-placement=completed`, `sample all result=passed`.
  - 성공 2: exit 0, `deliverydispatch-placement=completed`, `sample all result=passed`.

## .NET

- `framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:138-141`: 최초 배차의 offer row/deadline 생성을 `Assigned` publish 완료 뒤로 이동했다.
- `framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:200-203`: 재배차의 offer row/deadline 생성을 `Reassigned` publish 완료 뒤로 이동했다.
- `framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests/DeliveryDispatchRegressionTests.cs:65-92`: 최초 배차와 재배차가 각각 `publish → offer row/deadline → courier send` 순서를 유지하는 계약 테스트를 추가했다.
- focused test: `DeliveryDispatch_Offer_Deadline_Starts_After_Status_Publish` — 1 passed, 0 failed.
- 샘플: 지정된 `TMPDIR`, `ZLINK_LIBRARY_PATH`, compiler/node-reuse 설정과 `Systems.Zlink.0.17.0.nupkg` hash 기반 `NUGET_PACKAGES`를 사용해 `framework/languages/dotnet/samples/DeliveryDispatch/run_sample.sh` 실행.
  - 성공 1: exit 0, `deliverydispatch-placement=completed`.
  - 성공 2: exit 0, `deliverydispatch-placement=completed`.

## Node.js

- `framework/languages/node/samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts:62-83`: 최초 배차와 재배차가 공유하는 `startOffer`에서 상태 publish 완료 뒤 offer row/deadline을 만들고 즉시 courier actor로 전송한다.
- `framework/languages/node/test/contract/sample-regression.test.js:585-600`: `Assigned`/`Reassigned` publish 선택 뒤에 deadline 생성, store 저장, actor send가 이어지는 순서를 고정하는 계약 테스트를 추가했다.
- focused test: `node --test --test-name-pattern='DeliveryDispatch starts the offer deadline' test/contract/sample-regression.test.js` — 1 passed, 0 failed.
- 샘플: `TMPDIR=/dev/shm/zlink-tmp-node flock -w7200 /tmp/zlink-node-gate.lock bash samples/DeliveryDispatch.Ts/run_sample.sh`
  - 실행 1: exit 0, functional marker 3개와 placement marker 통과, `PASS DeliveryDispatch.Ts`.
  - 실행 2: exit 0, functional marker 3개와 placement marker 통과, `PASS DeliveryDispatch.Ts`.

## 재시도 기록

- C++ 최초 시도는 `dispatch actor route courier node 1` readiness marker가 0회여서 실패했다. 소스 변경 없이 재실행한 두 회는 모두 통과했다.
- .NET 최초 시도는 로컬 feed에 `Zlink.HttpClient.0.10.0.nupkg`가 없어 restore 단계에서 실패했다. 저장소의 `scripts/local-package/http-client/build-wsl.sh dotnet`으로 요구 패키지를 생성한 뒤 두 회 모두 통과했다.
- .NET 공유 gate lock은 다른 unit-test 작업이 장시간 점유하고 있었다. 해당 작업을 변경하지 않고 focused test의 artifacts 경로만 `.artifacts/codex/` 아래로 격리했다. 사용자 지시대로 `tests/Zlink.Framework.UnitTests`는 실행하지 않았다.

## BLOCKERS

- 없음.
