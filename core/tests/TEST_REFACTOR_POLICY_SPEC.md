# zlink Test Refactor Policy Spec

## Purpose

`core/tests`는 단순히 많은 테스트를 모아 두는 위치가 아니라, `core`
계약을 빠르게 검증하고 회귀를 명확히 드러내는 실행 표면이어야 한다.

이 문서는 `core/tests` 전체를 다시 구조화할 때 따라야 하는 기준 문서다.
핵심 목표는 아래 4가지다.

- 기본 개발 루프에서 빠르게 실패를 확인할 수 있을 것
- 테스트 커버리지는 줄이지 않고 중복만 제거할 것
- POSD 기준으로 테스트 구조 자체의 복잡도를 줄일 것
- 테스트 추가/수정 시 lane, split case, helper, 문서가 함께 일관되게 유지될 것

## Design Philosophy

이 저장소의 테스트도 John Ousterhout의 POSD 원칙을 따라야 한다.

테스트에 적용하는 핵심 원칙은 아래와 같다.

- 깊은 테스트 모듈을 선호한다.
  테스트 helper는 얕은 wrapper가 아니라, 반복되는 setup/sync/teardown
  복잡도를 실제로 숨기는 깊은 모듈이어야 한다.
- change amplification을 줄인다.
  동일한 시나리오 목록이 `main`, `CTest`, `README`, stress script에
  중복 정의되면 안 된다.
- hidden coupling을 줄인다.
  테스트가 transport timing, global env, 포트 충돌, teardown ordering에
  암묵적으로 기대지 않도록 한다.
- temporal decomposition을 경계한다.
  "connect 하고 나중에 ready 될 것" 같은 흐름을 테스트가 추측해서
  맞추면 안 된다. 테스트는 계약을 직접 기다리거나, 명시적 barrier를
  사용해야 한다.
- 얕은 테스트 중복을 제거한다.
  같은 기능을 transport, payload size, mode만 바꿔 여러 번 검증하는
  구조는 기본 lane에서 제거하고 대표 케이스만 남긴다.

## Test Portfolio Model

모든 테스트는 아래 6개 책임 중 정확히 하나를 주 책임으로 가져야 한다.

- `contract`
  public API, option, state, monitor, error surface의 계약 검증
- `behavior`
  대표 기능 시나리오 검증
- `topology`
  peer/discovery/node 상태 변화 검증
- `process-smoke`
  실행 파일 조합 수준의 대표 smoke 검증
- `scale-stress`
  scale, long sequence, high fanout, large payload
- `historical-regression`
  특정 버그 재현과 재발 방지

하나의 테스트가 여러 책임을 동시에 맡으면 커지고 느려진다.
그 경우 테스트를 쪼개고, 가장 무거운 축은 `regression`으로 이동한다.

## Lane Model

`core/tests`는 아래 4개 lane을 유지한다.

- `unittest`
- `integration`
- `e2e`
- `regression`

현재 실행기는 이 4개만 지원하므로, 추가 lane 이름을 늘리지 않는다.
대신 lane 내부의 정책을 더 엄격히 구분한다.

중요:

- 현재 저장소에는 `integration-fast`, `integration-heavy` 같은 별도 lane이 없다
- `integration`은 단일 lane이며, fast/heavy 구분은 lane 내부 정책으로만 관리한다
- heavy한 기본 기능 테스트는 `integration` 내부에 남길 수 있지만,
  scale, matrix, long sequence, historical flake는 `regression`으로 이동한다

### `unittest`

목적:

- 내부 순수 로직
- parser/decoder/state machine
- snapshot mapping
- access/ownership invariant

규칙:

- 네트워크와 실제 live socket timing에 의존하면 안 된다
- 전부 `parallel-safe`
- 포트, discovery, transport matrix 사용 금지

### `integration`

목적:

- 기본 개발 루프에서 확인해야 하는 핵심 계약과 대표 기능

규칙:

- 기본 transport 1개만 사용한다. 특별한 이유가 없으면 `tcp`
- 기본 payload 1개만 사용한다. 특별한 이유가 없으면 small payload
- 기본 mode 1개만 사용한다. callback/recv를 둘 다 검증해야 한다면
  split case 2개까지만 허용한다
- scale, long sequence, matrix, very-large payload는 금지
- process benchmark 스타일 테스트는 패턴당 대표 smoke 1개만 허용한다
- 모든 `integration` 테스트는 기본 lane 시간 예산 안에 들어야 한다

### `e2e`

목적:

- 실행 파일 조합, 대표 시나리오, child process orchestration smoke

규칙:

- representative scenario만 둔다
- 동일 기능의 split case 반복 등록을 최소화한다
- heavy discovery mesh, large scale, transport matrix는 넣지 않는다

### `regression`

목적:

- 장시간/대규모/flake 재현/역사적 회귀

규칙:

- 기본 개발 루프를 막아서는 안 된다
- scale, matrix, long sequence, high concurrency는 여기로 이동한다
- 기본 lane에서 시간이 과한 테스트는 먼저 `regression` 이동을 검토한다

## Execution Mode Policy

실행 모드는 아래 2개다.

- `parallel-safe`
- `serial`

### `parallel-safe`

아래 조건을 모두 만족해야 한다.

- global env mutation 없음
- live socket timing 민감도 낮음
- discovery/shared registry 의존 없음
- teardown ordering에 민감하지 않음
- 포트 충돌/외부 자원 lock 필요 없음

### `serial`

아래 중 하나라도 만족하면 `serial`이다.

- 실제 network socket bind/connect가 필요함
- discovery나 registry를 사용함
- child process를 띄움
- `RESOURCE_LOCK`가 필요함
- teardown ordering이 민감함

## Default Time Budget

기본 시간 예산은 lane 전체 기준으로 아래를 목표로 한다.

- `unittest` lane 전체: 10초 내
- `integration` lane 전체: 120초 내
- `e2e` lane 전체: 180초 내
- `regression`: 기본 lane 제외, 시간 제한 없음

개별 테스트 권장 상한은 아래를 목표로 한다.

- `unittest` 개별 test binary 또는 split case: 1초 미만, 길어도 3초 내
- `integration` 개별 test binary 또는 split case: 5초 내, 길어도 10초 내
- `e2e` 개별 binary: 30초 내, 길어도 60초 내
- `regression` 개별 test: 별도 상한 없음. 다만 목적과 비용이 문서화돼야 한다

해석 규칙:

- 기본 lane 시간 예산을 넘는 테스트는 이유 없이 유지하지 않는다
- 같은 기능을 유지하면서 더 짧은 representative scenario로 대체 가능하면
  기본 lane에서는 반드시 대체한다
- 개별 테스트가 권장 상한을 넘는다면, lane 총 시간과 별개로 구조를 재검토한다

## Duplication Policy

중복은 아래 순서로 제거한다.

### 1. 기능 중복

같은 기능을 아래만 바꿔 여러 번 돌리는 경우 기본 lane에서 제거한다.

- transport
- payload size
- recv mode
- single/multi
- thread count
- client count

기본 lane에는 representative case 하나만 남긴다.

### 2. Surface 중복

같은 기능을 아래 두 surface가 동시에 검증하면 하나를 줄인다.

- top-level binary `main`
- CTest split case

원칙:

- binary smoke는 대표 조합만
- split case는 빠른 단위 검증용만

### 3. Documentation 중복

동일한 테스트 목록이 아래 위치에 제각각 적혀 있으면 안 된다.

- test source `main`
- `core/tests/CMakeLists.txt`
- `core/tests/README.md`
- stress/TSAN shell script

가능하면 source case list를 기준으로 CMake split spec이 따라가고,
README와 shell script는 최소 설명만 유지한다.

## Coverage Preservation Rule

중복 제거는 허용하지만 커버리지 축 제거는 금지다.

아래 축은 유지돼야 한다.

- 기능 축
- mode 축
- topology 축
- invalid/error 축
- teardown/lifecycle 축
- representative process-smoke 축

패턴별 최소 필수 커버리지는 아래를 항상 유지한다.

- `basic`
  대표 정상 동작 1개
- `invalid`
  대표 오류/거부 계약 1개
- `teardown`
  destroy/close/order/ownership 관련 대표 케이스 1개
- `status-or-topology`
  status snapshot, monitor contract, topology, routing-id, ownership 중
  그 패턴에 맞는 관찰 surface 1개

예:

- `SPOT`에서 `tcp/ws/tls/wss`를 모두 기본 lane에 둘 필요는 없다
- 그러나 `basic delivery`, `callback isolation`, `discovery interop`,
  `invalid usage`, `status snapshot` 같은 기능 축은 남아 있어야 한다

## Pattern-Specific Policy

### SPOT

기본 lane에 남길 최소 세트:

- node/handle create-destroy and status
- direct tcp delivery
- multi publisher representative case
- callback or recv isolation 대표 case
- discovery interop 대표 case
- invalid usage / ownership / default-handle contract

SPOT 최소 필수 체크리스트:

- `basic`: direct tcp delivery
- `invalid`: unsupported surface or ownership violation
- `teardown`: node/handle create-destroy
- `status-or-topology`: node status snapshot or discovery interop

기본 lane에서 제외할 대상:

- repeated transport matrix
- WSS/TLS permutations
- large multi-node mesh
- historical first-delivery flakes
- large scale process barriers

### Raw PUB/SUB

기본 lane에 남길 최소 세트:

- basic delivery
- topic isolation
- callback/recv representative case
- reconnect 대표 case

PUB/SUB 최소 필수 체크리스트:

- `basic`: basic delivery
- `invalid`: invalid subscription/usage or option rejection 대표 1개
- `teardown`: close path representative 1개
- `status-or-topology`: connection-ready or reconnect representative 1개

제외 대상:

- payload/transport matrix
- repeated subscription fanout scale

### DEALER/ROUTER

기본 lane에 남길 최소 세트:

- basic echo
- routing-id contract
- mandatory/HWM representative case

DEALER/ROUTER 최소 필수 체크리스트:

- `basic`: basic echo
- `invalid`: mandatory or invalid routing contract 대표 1개
- `teardown`: close path representative 1개
- `status-or-topology`: routing-id or ready contract 대표 1개

제외 대상:

- large sequence process benchmark
- transport matrix
- heavy TLS window sequences

### STREAM

기본 lane에 남길 최소 세트:

- basic socket scenario
- threadsafe representative case
- blocking wakeup representative case

STREAM 최소 필수 체크리스트:

- `basic`: basic socket scenario
- `invalid`: invalid send/recv contract representative 1개
- `teardown`: close/shutdown representative 1개
- `status-or-topology`: ready gating or threadsafe representative 1개

제외 대상:

- duplicated ready-gating regressions beyond one representative case
- repeated matrix/large payload sequences

## Process Benchmark Test Policy

`integration/monitoring/*benchmark_process.cpp` 류는 다음 원칙을 따른다.

- 기본 lane에는 패턴당 대표 smoke만 둔다
- 대규모 client count, long payload sequence, TLS/WSS matrix는
  `regression`으로 이동한다
- process benchmark는 core bug 증명 수단이 아니다
- core bug는 먼저 `core/tests/`의 direct regression으로 재현돼야 한다

## Synchronization Policy

모든 테스트는 fail-fast 규칙을 따른다.

- retry loop 금지
- sleep 기반 동기화 금지
- hard timeout 사용
- timeout이면 즉시 실패

테스트가 기다릴 수 있는 건 아래 둘뿐이다.

- public contract event/state
- test-owned explicit barrier protocol

테스트가 내부 구현 timing을 추측해서 기다리면 안 된다.

## Helper Refactor Policy

`testutil*`은 아래 기준으로만 확장한다.

- setup boilerplate를 의미 있게 줄일 것
- sync/teardown invariants를 명확히 감출 것
- 여러 패턴에서 재사용될 것

추가 금지 대상:

- 특정 테스트 한 곳에서만 쓰는 얕은 wrapper
- 이름만 바꾼 thin helper
- hidden global state에 의존하는 helper

우선 통합 대상:

- monitor ready wait helper
- process barrier helper
- spot node/handle helper
- split-case registration helper

## CTest Registration Policy

CTest 등록은 아래 규칙을 따른다.

- top-level binary와 split case가 같은 기능을 과도하게 중복 등록하지 않는다
- 기본 lane에 포함되는 split case는 binary의 최소 시나리오와 같은 범위여야 한다
- `regression` 이동 대상은 `integration`에서 반드시 제거한다
- 개별 split test 이름은 기능을 설명해야 하고 transport/payload/mode를
  필요한 범위에서만 드러낸다

## Refactor Procedure

테스트 리팩토링은 아래 순서로 진행한다.

1. inventory 작성
   각 테스트를 `contract / behavior / topology / process-smoke /
   scale-stress / historical-regression` 중 하나로 태깅한다

2. duplicate cluster 식별
   기능 중복, transport 중복, split 중복을 묶는다

3. 기본 lane 최소 세트 확정
   패턴별 대표 시나리오만 남긴다

4. regression 이동
   long, flaky, matrix, scale를 `regression`으로 이동한다

5. helper 통합
   동일 wait/setup/teardown helper를 통합한다

6. 문서와 runner 정리
   `README`, shell runner, CMake split spec을 같은 구조로 맞춘다

7. lane 시간 측정
   기본 lane가 시간 예산 안에 들어올 때까지 반복한다

## POSD Review Loop

테스트 리팩토링은 1회 변경으로 끝내지 않는다.
아래 루프를 반복하고, 더 이상 줄일 항목이 없을 때 종료한다.

- 리뷰
  중복, 얕은 helper, dead test, 과한 matrix, stale split case 탐지
- 단순화
  기본 lane 최소 세트 유지, 나머지 제거 또는 regression 이동
- 검증
  `run_test_lanes.sh`와 대표 split case 통과 확인
- 재리뷰
  문서/CTest/helper/source가 다시 중복되지 않았는지 확인

종료 조건:

- 기본 lane 시간 예산 만족
- 패턴별 최소 세트가 명확함
- 문서와 실제 등록 목록이 일치함
- dead code, stale split case, obsolete helper가 더 이상 남아 있지 않음

## Non-Goals

이 문서는 아래를 목표로 하지 않는다.

- 모든 transport 조합을 기본 lane에 유지하는 것
- 모든 historical flake를 기본 lane에서 계속 돌리는 것
- process benchmark를 core correctness의 주 surface로 쓰는 것
- 테스트 개수 자체를 유지하는 것

테스트 품질의 기준은 개수가 아니라,
짧고 명확한 대표 시나리오가 계약 축을 얼마나 깊게 커버하는가다.
