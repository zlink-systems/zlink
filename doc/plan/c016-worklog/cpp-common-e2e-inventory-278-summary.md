# C++ common E2E inventory 검증 기록

감독이 설정별 구현 범위, 공통 계약과 실제 실행 증거, 남은 제약을 확인하기 위한 작업 기록이다.
작업 범위는 C++ E2E source·runner·feature map과 이 파일이다. Runtime 수정, 보호 문서 수정과 commit은 하지 않는다.

## 기준 실행

`ctest --test-dir build -R test_cpp_framework_common_e2e_inventory --output-on-failure`:
FAIL. 14 configs, 361 scenarios, feature-map-missing=94, source-missing=122,
incomplete-status=62, 합계 278. 원본 로그: `/tmp/cpp-inventory-baseline.log`.
표의 수는 `map/source/status` 순서이며, 한 scenario에 여러 조건이 중복될 수 있다.

| Config | 기준 | 현재 | 분류·변경 파일 | 실행 결과 |
|---|---|---|---|---|
| 1 RegistryMessaging | 0/0/0 | 0/0/0 | 변경 없음 | inventory PASS |
| 2 SpotService | 11/15/0 | 11/15/0 | 조사 중 | 미실행 |
| 3 PubSub | 10/17/5 | 10/17/5 | 조사 중 | 미실행 |
| 4 RegistrationCodec | 0/0/0 | 0/0/0 | 변경 없음 | inventory PASS |
| 5 ResilienceLifecycle | 17/18/1 | 17/18/1 | 조사 중 | 미실행 |
| 6 DiscoveryRegistryHa | 14/14/2 | 14/14/2 | 조사 중 | 미실행 |
| 7 RuntimeMonitoring | 1/1/2 | 1/1/2 | 조사 중 | 미실행 |
| 8 AutomaticTurnDispatch | 5/14/0 | 5/14/0 | 조사 중 | 미실행 |
| 9 ToActorMessaging | 0/0/0 | 0/0/0 | 변경 없음 | inventory PASS |
| 10 SpotActorTransfer | 2/22/23 | 2/22/23 | 조사 중 | 미실행 |
| 11 ObservabilityOps | 3/8/11 | 3/8/11 | 조사 중 | 미실행 |
| 12 ChannelEgressRouting | 0/0/0 | 0/0/0 | 변경 없음 | inventory PASS |
| 13 SubmitAdmission | 0/13/18 | 0/13/18 | 조사 중 | 미실행 |
| 14 InstanceSpot | 31/0/0 | 31/0/0 | A: ID 등록만 있고 client가 미구현 오류로 끝나는 항목 존재 | 미실행 |

## 분류 기준과 교차언어 대조

- A: 계약의 절차 또는 assertion이 없는 scenario. ID 문자열이나 일반 request 성공은 완료 증거가 아니다.
- B: 기존 실행 절차와 assertion이 같은 계약을 충족하고 등록·표만 어긋난 scenario.
- STOP: C++ public API로 표현할 수 없는 계약. Fixture 구현 부재와 구분한다.
- Java InstanceSpot runner도 일부 selector를 `blocked_scenario`로 거부한다.
  .NET runner도 지원하는 process fixture를 제한한다. 다른 언어의 inventory PASS 자체를
  scenario 동작 완료의 근거로 사용하지 않는다.

## BLOCKERS

조사 중. Framework feature가 필요하면 spec 조항과 public API 근거를 여기에 기록하고
해당 scenario의 구현을 멈춘다. 미검증 항목을 완료로 표시하지 않는다.

## 검증

- 전체 build: 진행 중, `/tmp/cpp-common-e2e-build.log`.
- C++ unit ctest: 미실행.
- Runtime 변경: 없음. 수정 전/후 runtime 규칙 수: 변화 없음.
