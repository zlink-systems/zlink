# 규칙 — lane 소유 동시성 전환

> 진행 상태는 [progress.ko.md](progress.ko.md)의 단일 진행표에서만 갱신한다.
> 이 문서는 **바뀌지 않는 규칙과 프로토콜, 그리고 발견 로그**를 담는다.

## 1. 진행 방식 (확정)

- **역할**: 전환·조사 = codex(조사 sol, 구현 terra) · 감독/리뷰/판정/커밋/게이트 = Claude.
- **2패스 원칙**: ①동작 보존 lane 전환(본문 무변경) → 커밋 → ②posddd 리팩토링
  (할당·복사·경합·죽은 코드, `doc/principal/dev/posddd.ko.md`) → 별도 커밋.
  ②는 측정 우선순위가 낮으면 근거를 남기고 생략할 수 있다.
- **가속 모드**(2026-08-26 사용자 승인): 서로 다른 파일을 만지는 클래스는 codex 잡을
  병렬로 돌린다. 같은 트리의 빌드·테스트는 트리 락(flock)으로 직렬화한다.
- **게이트**: 스펙 06 §10 — 단위 테스트 실패 0, 6샘플 placement marker 유지
  (ZoneWorld는 기존 결함으로 제외), 관측 동작 불변.
- **푸시**: 2~3클래스 누적마다.

### 일을 어떻게 자르는가

| 단위 | 크기 | 무엇으로 끝나는가 | 누가 |
|---|---|---|---|
| **요청 1건** | 클래스 1개 (대형은 2개 묶음) | 에이전트 보고 (CP1) | codex 1잡 |
| **커밋 1건** | 요청 1건 | 리뷰 승인 후 커밋 | Claude |
| **배치** | 2~4 요청 | 전체 unit + 6샘플 게이트 → 푸시 (CP2) | Claude |
| **마일스톤** | 언어 전체 | §3 전 항목 (CP3) | Claude |

**병렬은 요청 단위로만 한다.** 게이트와 커밋은 항상 중앙에서 한 번씩이다.

## 2. 언어 병렬 단계

.NET이 규칙의 정본이다(정본-우선 포팅 정책). 단계마다 진입 조건이 있고, 조건을 만족하면
언어끼리는 서로 기다리지 않고 병렬로 간다.

- **L0 primitive** — 진입: 스펙 06 확정. golden은 .NET `StateLaneTests` 14개, 언어 관용구로
  옮기되 검증하는 사실 불변. node는 동시성 항목 제외(단일 스레드 — 재진입 검출이 가치).
- **L1 표본 1개** — 진입: 해당 언어 L0 + dotnet 중형 구간(#5·6) 완료. 목적은 진도가 아니라
  그 언어 고유 함정의 조기 노출.
- **L2 전면 확산** — 진입: 해당 언어 L1 완료 + **dotnet 마일스톤 게이트 통과**. 이유: 규칙집이
  아직 움직인다(발견 4·5·6이 하루에 나왔다) — 확정 전 확산은 재작업을 만든다.

### 빌드 트리는 4개뿐이다

| 트리 | 언어 | 게이트 락 |
|---|---|---|
| dotnet | dotnet | `flock /tmp/zlink-dotnet-gate.lock` |
| cpp | cpp | `flock /tmp/zlink-cpp-gate.lock` |
| node | node | `flock /tmp/zlink-node-gate.lock` |
| JVM | **java + kotlin 공유** | `flock /tmp/zlink-jvm-gate.lock` |

**java와 kotlin은 절대 동시에 빌드하지 않는다** (Gradle 트리 공유 — 동시 빌드 시
`Unresolved reference` 실증). 동시 실행 상한은 4다.

### 병렬 편성 규칙

- 한 트리에 codex 잡 여러 개는 서로 다른 파일을 만질 때만. 빌드·테스트는 트리 락으로 직렬화.
- cpp는 codex 안전 필터가 메모리 수명 주제를 거부한 전례 2회 — 거부되면 Claude 서브에이전트로 우회.
- codex 직접 기동 시 **`< /dev/null` 필수** — stdin이 열려 있으면 "Reading additional input
  from stdin..."으로 무한 대기한다(2026-08-26 실증, 잡 4개 동시 스톨).

## 3. 마무리 항목 — 모든 언어 공통 (해당 언어 전환 후)

- **호환 경계 회수** — 블로킹 브리지를 async 전파로 해소, 남으면 사유 기록.
  dotnet `AwaitStateLane` / java `inStateLane` `join()` / cpp `lane.run().get()`(동기 표면이
  설계인 곳은 사유 기록) / node 해당 없음.
- **posddd ②패스 잔여** — 각 언어 survey 관찰 목록 기준.
- **마일스톤 게이트 (언어별)** — 전체 unit·계약 + 샘플 게이트, async 경계 스냅샷 계열 재측정,
  codex sol 마일스톤 리뷰. dotnet 마일스톤 추가: §8 기존 결함 판정, 스펙 06 개정(§7 일괄 반영).
- **ZoneWorld 결함 해소 (§3.1)** — 캠페인 최종 단계.

### 3.1 ZoneWorld 최종 단계 진행 규칙 (2026-08-26 사용자 지시)

- CP3 판정("하위 증상인가")에서 끝내지 않는다 — Z1~Z3를 직접 수정하고 Z4로 게이트에
  복귀시키는 것까지가 완료 조건. lane 전환으로 자연 해소된 항목은 반복 실행으로 재현 불가
  확인 후 닫는다.
- **Z0 — cross-language e2e 선행**: 샘플 작업 전에
  `framework/languages/cpp/cross-language/run_cross_language_smoke.sh`
  (`ZLINK_CPP_BUILD_DIR=../build`, 모든 언어 락 단독 점유)를 먼저 그린으로.
- **샘플은 하나씩**: 감독관 agent(Claude)가 샘플 1개 단위로 작업을 요청하고, 완료되면
  감독관이 리뷰한 뒤 다음으로. codex에 7개를 한 번에 맡기지 않는다.

## 4. 게이트 매트릭스

**게이트는 언제나 Claude가 중앙에서 돌린다.** 에이전트 보고가 여러 번 사실과 달랐다.

| 언어 | 단위·계약 | 7샘플 일괄 | 비고 |
|---|---|---|---|
| dotnet | `dotnet test tests/Zlink.Framework.UnitTests` | `framework/languages/dotnet/samples/run_samples.sh` | 기준 1893+ / 실패 0 |
| cpp | `ctest --test-dir build -L 'framework-(unit\|contract)' -LE 'e2e\|sample\|perf'` | `framework/languages/cpp/samples/run_samples.sh` | flake: Bingo 후반 ~1/5, TTT teardown ~1/15. exit 86/134는 1회 재실행 |
| java | `./gradlew :zlink-framework-core:test` | `framework/languages/java/samples/run_samples.sh` | 러너가 java→kotlin 순차 |
| kotlin | `./gradlew :zlink-framework-kotlin:test` | (같은 러너) | `ZLINK_SAMPLE_LANGUAGES`로 분리 가능 |
| node | `npx tsc -b tsconfig.build.json --force` 후 `node --test test/contract/*.test.js` + verify:m6a/b/c | `framework/languages/node/samples/run_samples.sh` | **`--force` 필수** |

cross-language e2e: `framework/languages/cpp/cross-language/run_cross_language_smoke.sh`
(`ZLINK_CPP_BUILD_DIR=../build`) — 모든 언어 락을 잡고 단독으로. CP3/Z0 전용.

체크포인트: CP1(요청 1건)=에이전트 집중 테스트, CP2(배치)=Claude 단위·계약+7샘플,
CP3(마일스톤)=+cross-language e2e+스냅샷 재측정.

### 알려진 기존 실패 (게이트 판정 제외 — 회귀로 읽지 말 것)

- **ZoneWorld**: cpp split-brain·dotnet mesh admission (Z1·Z2). CP3에서 판정, §3.1에서 수정.
- **cpp `test_cpp_framework_layout_contract`**: ShoppingMall OrderWorkflow main.cpp L350·L446
  blocking `result()` 지문 — base `3cbfbde4f9`부터. 샘플 수정은 별도 작업.
- **node `verify:m6c-runtime` 2건** (stash 대조로 baseline 동일 110/112 확인): ① legacy fence
  불완전 시 ProtocolError 기대 vs actorType 경로(m6c-actor-join-store-resolution L126 vs
  remote-actor-join-receiver L63) ② retain identity의 coordinator fence 기대 vs codec 의도적
  제외(m6c-relocation-wire-codec L415 vs service-stateful-wire-codec L354). 계약 판정 필요 —
  별도 작업.
- **java full-run flake**: `ZLinkJavaRawMeshNodeM6ATest.descriptorBackedPeerIntent…`,
  `ZLinkAsyncSerialQueueTest.queuedRelocationIntent…` — 단독 재실행 통과 확인 후 무시.
- 간헐 실패는 이 목록과 대조 전에는 회귀로도 flake로도 단정하지 않는다.

## 5. 작업 주기 — 요청 → 보고 → 리뷰

### 5.0 요청 템플릿 (그대로 복사해 쓴다)

```
# 공통 규칙 (state lane 전환)

작업 디렉터리: /home/hep7/project/zlink (브랜치 refactor/lane-ownership-concurrency)

먼저 읽기:
  ① framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md (규칙 스펙)
  ② doc/plan/concurrency-redesign/sample-conversion-report.ko.md (함정: AsyncLocal 상속 → SuppressFlow, out 실패부가값)
  ③ doc/plan/concurrency-redesign/conversion-order-survey.ko.md 의 해당 클래스 절
  ④ Runtime/Execution/ZLinkStateLane.cs
  ⑤ doc/plan/concurrency-redesign/rules.ko.md §7 발견 목록 (4·5·6 필수)

절차: 재진입 자리를 private 분리 → lock 제거·본문을 _lane.RunAsync로 감싸기(본문 로직 한
글자도 불변) → lane 안 장기 작업은 시작점에 SuppressFlow(+동기 프리픽스면 Task.Run, 발견4) →
out은 스펙 06 §7대로 반환 타입화 → 호출자 전파(허용 파일 밖으로 번지면 AwaitStateLane 호환
경계, 개수 보고) → 직접 단위 테스트 시그니처 추종(assertion 의미 불변).

금지: ConcurrentDictionary/SemaphoreSlim 치환, posddd 리팩토링(별도 패스), spec/** 수정,
git 명령 전부, 관측 동작 변경, 허용 파일 밖 수정.

빌드·테스트 직렬화: flock -w 7200 /tmp/zlink-<tree>-gate.lock <명령>
전체 게이트는 마지막에 1회만, 실패 0 확인 후 완료 선언.

보고: 재진입 실측·SuppressFlow 지점(동기 프리픽스 판정 포함) / 시그니처·전파·호환 경계 수 /
본문 조정 목록(없으면 없음) / 테스트 결과(숫자) / 예상과 달랐던 점 / 걸린 시간.

# 대상: <클래스명>

<파일 경로>
(lock <n>, <분류> — <근거>. 조사: <장기 작업/재진입/특이점>. 호출자: <파일 목록>.)

허용 파일: 대상, 위 호출자 <n>파일, 해당 단위 테스트.
```

**대상 절의 내용은 지어내지 않는다** — survey의 해당 클래스 절에서 옮긴다. 조사가 없으면
조사부터 한다.

### 5.1 CP1 보고 필수 항목 (빠지면 반려)

변경 파일 목록 / lock 전후 / 재진입 몇 곳·해소 방식 / 호환 경계 수·사유 / 단위 테스트 숫자 /
새 유형 여부 / 걸린 시간.

Claude 리뷰: 기계 변환=금지 패턴 grep+표본 정독, 판단 변환=구간 전문 정독.
금지 패턴: lane 밖 스냅샷 잔존, `.Result`/`.Wait()` 신규, C2의 ConcurrentDictionary 치환,
관측 동작 변경(순서·타임아웃·오류 코드), **테스트 기대값 변경**(#7에서 실제 반려).

### STOP 조건 (진행하지 말고 감독관에게 올린다)

- 관측 가능한 동작을 바꿔야만 통과되는 경우 → 스펙 문제일 수 있다. 임의 변경 금지
- 재진입이 구조적이라 lane 분할·경계 재설계가 필요한 경우
- 게이트 간헐 실패 → §4 기존 실패 목록과 대조 전 단정 금지

## 6. 지표 재측정 명령 (CP2마다 — progress 지표 행 갱신)

```bash
cd framework/languages/dotnet/src/Zlink.Framework
grep -rhoE 'lock *\(' --include=*.cs . | wc -l                        # lock 총계
grep -rhoE 'private readonly object _gate' --include=*.cs . | wc -l    # _gate 보유 클래스
grep -rlE 'ZLinkStateLane' --include=*.cs . | wc -l                    # lane 사용 클래스
grep -rhoE 'AwaitStateLane' --include=*.cs . | wc -l                   # 호환 경계(부채)
```

## 7. 전환 중 발견 (스펙 후보 — 마일스톤에서 스펙 06에 일괄 반영)

- **(1, 표본)** lane 안에서 시작한 timeout 작업의 AsyncLocal 소유권 상속 → SuppressFlow 처방.
  스펙 06 §6 유형 ②로 반영 완료.
- **(2)** gate 안에서 lane 블로킹 대기 시 데드락 판정 기준: lane 큐 항목이 그 gate를
  재획득하지 않고, 완료 TCS가 전부 `RunContinuationsAsynchronously`면 안전.
- **(4, 유형② 보완)** `SuppressFlow`는 스레드 전환 이후에만 유효 — 동기 프리픽스가 lane에
  재진입하면 `Task.Run`으로 시작 자체를 밀어낸다(ZLinkSpotNodeCatalog 실측, unit 128 연쇄).
  선행 await(Task.Delay 시작)면 SuppressFlow 단독 충분. CP1 체크리스트 항목.
- **(5, java L0 결함 — 수정 완료)** java `CompletableFuture.complete`는 비동기 아닌 dependent를
  완료 스레드에서 inline 실행 → CURRENT ThreadLocal 스코프 안 완료는 거짓 재진입을 만든다.
  처방: completeAsync 등으로 완료를 스코프 밖으로. kotlin·node·cpp 해당 없음.
  L0 리뷰 체크리스트: "완료 신호의 continuation이 어느 문맥에서 실행되는가".
- **(6, 유형 ③ 후보 — #7 실측)** lock 안에서 외부 콜백을 호출하던 자리. 콜백이 public 표면을
  재호출해도 Monitor 재진입으로 동작했다. lane 처방: turn A(검증+산출+**상태 전이 전부**+
  placeholder claim) → 콜백(turn 밖) → turn B(placeholder→task 교체만). 전이를 turn A에
  넣어야 콜백 창의 경쟁 관측자가 원본의 "lock 이후" 상태를 본다(#7에서 기대값 변화로 실증).
- **(7, #8 판정)** 외부 await를 품는 원자 async 임계 구역(SemaphoreSlim)은 상태 보호가 아니라
  **작업 프로토콜 직렬화**로 재해석해 semaphore를 유지하고, 그 안의 상태 변이만 lane turn으로
  감싼다(#5·6 _socketLifecycleGate 전례). lane turn 안에서 semaphore 획득 대기는 금지(역방향만 허용).
