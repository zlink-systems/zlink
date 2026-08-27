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
- **L2 전면 확산** — 진입: 해당 언어 L1 완료 + **dotnet 마일스톤 게이트 통과**.
  **완화(2026-08-26 감독관 판정, 사용자 일정 단축 요구)**: 발견 1~7이 이 문서에 성문화된
  시점부터 **저위험 후보(각 survey 제안 순서의 앞쪽)는 마일스톤 전에 병렬 개방**한다.
  이후 규칙 변경 시 해당 소형 재작업을 감수한다. 대형·고재진입 후보는 여전히 마일스톤 후.
- **소형 배치 가속(동 판정)**: 배치 잡은 집중 테스트만(CP1). 전체 unit·샘플은 중앙이
  4~5배치 묶어 1회(CP2).

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
| java | `./gradlew :zlink-framework-core:test` | `framework/languages/java/samples/run_samples.sh` | 러너가 java→kotlin 순차. **기준 1149/실패 0**(2026-08-27 cleanTest 실측) |
| jvm 추가 모듈 | `:zlink-stream-connector:test`(123) · `:zlink-framework-locations-redis:test`(27) | — | **2026-08-27 캠페인 범위 편입** — jvm 결함 6건 수정이 이 두 모듈을 건드렸다. 종전 매트릭스에 없었다 |
| kotlin | `./gradlew :zlink-framework-kotlin:test` | (같은 러너) | `ZLINK_SAMPLE_LANGUAGES`로 분리 가능 |
| node | `npx tsc -b tsconfig.build.json --force` 후 `node --test test/contract/*.test.js` + verify:m6a/b/c | `framework/languages/node/samples/run_samples.sh` | **`--force` 필수** |

cross-language e2e: `framework/languages/cpp/cross-language/run_cross_language_smoke.sh`
(`ZLINK_CPP_BUILD_DIR=../build`) — 모든 언어 락을 잡고 단독으로. CP3/Z0 전용.

**Z0 함정 (2026-08-27 발견 — 반드시 확인)**

- **java cross-language Host는 소스가 아니라 발행된 maven 산출물에 의존한다.**
  `cross-language/Host/build.gradle.kts`가 `systems.zlink:zlink-framework-core:0.10.0`을 쓰고,
  설치본(`build/install/.../bin/zlink-cross-language-host`)이 그 jar를 물고 있다. 발견 시점의
  설치본은 **2026-08-25자**였다. **`./gradlew publishToMavenLocal`을 선행하지 않으면 java
  스테이지가 수정 이전 코드로 통과 판정을 받는다.** cpp host도 타겟 재빌드가 필요하다.
- **`MSBUILDDISABLENODEREUSE=1`을 반드시 설정한다** (2026-08-27 실증). "dotnet 무관 스테이지만
  고르면 dotnet은 안 돈다"는 가정이 틀렸다 — `relocation` 등 일부 스테이지가 내부적으로 dotnet
  테스트 호스트를 빌드하고, MSBuild 노드가 재사용을 위해 죽지 않고 **언어 락 3개를 계속 붙든다**.
  실측: `relocation`이 rc=0으로 끝났는데 MSBuild 노드 6개가 cpp·jvm·node 락을 9분 넘게 쥐고 있어
  다음 스테이지(`java-cross`)가 진입하지 못했다. 해당 PID를 kill하니 즉시 진입했다.
  다른 세션이 dotnet을 쓰는 중이면 충돌 위험도 있다.
- 스테이지는 `ZLINK_CPP_CROSS_LANGUAGE_STAGE`로 선택한다(기본 `all`). 전체 20개 중 **9개가
  dotnet을 상대**로 한다. dotnet 무관 10개:
  `spot-route` · `message-follow` · `relocation` · `java-cross` ·
  `user-spot-join-{cpp-java, java-cpp, cpp-node, node-cpp, java-node, node-java}`.
  dotnet이 다른 세션 소관이거나 락이 점유된 경우 이 10개로 cpp·node·jvm 간 wire 계약을 덮는다.

체크포인트: CP1(요청 1건)=에이전트 집중 테스트, CP2(배치)=Claude 단위·계약+7샘플,
CP3(마일스톤)=+cross-language e2e+스냅샷 재측정.

### 알려진 기존 실패 (게이트 판정 제외 — 회귀로 읽지 말 것)

- **ZoneWorld**: cpp split-brain·dotnet mesh admission (Z1·Z2). CP3에서 판정, §3.1에서 수정.
- **cpp `test_cpp_framework_host_lifecycle` 간헐 실패** (2026-08-27 이분 실증): 별도 worktree의
  **기준선(커밋된 배치 상태)에서 ×3 중 1회 실패**했다. 즉 `public_host_runtime`·`spot_runtime`
  전환이 만든 회귀가 아니라 원래 있던 flake다. 중앙 게이트 1차에서 실패해도 개별 재실행에서
  통과하면 flake로 판정한다(실측: 18:03 1차 실패 → 재실행 rc=0).
  **주의**: 이 항목은 세션 중 "격리 ①에서 실패 목록에 없었으니 이후 전환이 원인"으로 오판된
  전례가 있다. 한 번 통과한 것은 flake가 아니라는 증거가 되지 못한다 — 기준선에서 반복 측정해야 한다.
- **cpp `test_cpp_framework_m6b_runtime`**: Subprocess aborted(exit 86/134 계열). 재실행 통과 확인 후 무시.
- **cpp `test_cpp_framework_layout_contract`**: ShoppingMall OrderWorkflow main.cpp L350·L446
  blocking `result()` 지문 — base `3cbfbde4f9`부터. 샘플 수정은 별도 작업.
- **node `test/contract/*` 23건** (2026-08-27 기준선 대조로 확정): 계약 스위트 전수 실행 시
  1533건 중 **23건 실패**. node 결함 5건 수정을 되돌린 base 상태와 **실패 이름 집합이 완전히 동일**
  (기준선에만 있는 것 0·현행에만 있는 것 0, 수정 후에는 +5 테스트/+5 통과/실패 동수).
  즉 전부 캠페인 이전부터 있던 기존 실패다. 계열: 샘플 게이트(Bingo·GameQuest·ShoppingMall·
  ZoneWorld·run_samples 자체검사), stream/session teardown·통지, relocation seal, spot managed timer,
  canonical spec 트리. **이 스위트를 캠페인 중 전수로 돌린 기록이 없어 그동안 계수되지 않았다.**
  개별 판정·수정은 캠페인 범위 밖 — 별도 작업으로 이월.
- **node `verify:m6c-runtime` 2건** (stash 대조로 baseline 동일 110/112 확인): ① legacy fence
  불완전 시 ProtocolError 기대 vs actorType 경로(m6c-actor-join-store-resolution L126 vs
  remote-actor-join-receiver L63) ② retain identity의 coordinator fence 기대 vs codec 의도적
  제외(m6c-relocation-wire-codec L415 vs service-stateful-wire-codec L354). 계약 판정 필요 —
  별도 작업.
- **java full-run flake**: `ZLinkJavaRawMeshNodeM6ATest.descriptorBackedPeerIntent…`,
  `ZLinkAsyncSerialQueueTest.queuedRelocationIntent…` — 단독 재실행 통과 확인 후 무시.
  **`ZLinkJavaRawSpotNodeM6BTest.remoteSpotSendAndRequestUseTheExactRouteFence` 추가**
  (2026-08-27 실증): jvm 결함 6건 수정 잡의 full-run에서 1회 실패(`ExecutionException:
  ZlinkRequestException`, `ZLinkJavaRawMeshNode.requestSpot:1812`). 같은 트리에서 **단독 ×3
  전부 통과 + 전체 core 스위트 ×2 전부 통과 = 5/5 그린**으로 재현 실패. 수정 6건의 호출
  스택에도 없다. 위 두 건과 같은 full-run 부하 한정 flake로 판정.
- **cpp TicTacToe `JoinGameNotify` 간헐 유실도 기존 결함이다** (2026-08-27 worktree 이분 실증 —
  배치9 이전 시점에서 6회 중 4회 동일 실패): detached one-way bound-session 전달이 remote
  submit 성공을 delivery로 간주, owner 측 stream write 미확인. 근본 수정은 owner-side delivery
  계약 설계 필요(스펙 판정 대상) — 캠페인 범위 밖 이월. 가설 수정 2회는 실측 기각·원복.
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

### 5.2 측정 규율 (2026-08-27 실패 3회로 성문화)

에이전트 보고를 믿지 않는 것만으로는 부족하다. **중앙 측정 자체가 틀릴 수 있다.**

- **같은 빌드 트리에 codex 잡을 2개 이상 동시에 돌리지 않는다.** §2의 "서로 다른 파일을
  만질 때만"은 **부족한 조건이었다**(2026-08-27 실증). 파일이 달라도 잡들은 **같은 빌드 산출물을
  공유**하고 서로의 편집 중간 상태를 물고 컴파일한다. cpp 대형 3잡을 병렬로 돌렸더니 ctest
  5건 파손(`execution` SEGFAULT·`m6b`·`m6c`·`actor_gateway`)이 관측됐으나, 패치로 저장해
  **순서대로 얹으니 전 단계가 기존 실패 1건만** 남겼다 — 파손은 전부 측정 산물이었다.
  진짜 병렬이 필요하면 **잡마다 별도 worktree**를 준다(빌드 디렉터리만 분리해서는 부족하다 —
  서로의 소스를 컴파일하기 때문이다).
- **에이전트가 같은 트리를 빌드하는 동안 중앙 게이트를 돌리지 않는다.** 트리 락은 빌드끼리만
  직렬화하고 빌드-테스트 사이는 보호하지 않는다. 반쯤 갱신된 바이너리를 물면 실패가 가짜다.
  게이트 전에 해당 트리를 건드리는 프로세스가 없는지 확인한다.
- **잡 실행 중 `git add -A` 금지.** 진행 중 작업이 리뷰 없이 커밋에 유입된다(실증). 경로를 명시한다.
- **잡 스크립트는 에러 출력을 버리지 않는다.** `> /dev/null 2>&1`로 빌드 에러를 버리면 실패
  원인을 알 수 없고, 무관한 원인(예: 새 worktree의 protobuf 코드젠 누락)에 속아 잘못된 결론을 낸다.
- **전체 빌드 rc로 판정하지 않는다.** 필요한 타겟만 빌드하고 `Built target <name>` 같은 성공
  문자열을 직접 확인한다. 무관한 타겟(e2e·샘플) 실패에 판정이 흔들리면 안 된다.
- **파이프라인의 `$?`는 마지막 명령의 것이다.** `cmd | tail` 뒤의 `$?`는 `tail`의 rc다.
  `PIPESTATUS`를 쓰거나 파이프를 분리한다.
- **회귀 판정은 별도 worktree에서.** 본체는 잡들이 계속 건드린다. 기준선 worktree에
  `.artifacts` 심링크를 걸고(cpp·dotnet 필수) 단계별 누적으로 이분한다. 각 단계 **×3 이상**.
- **게이트가 실제로 실행됐는지 확인한다.** 종료 코드 0은 "통과"가 아니라 "명령이 성공"일 뿐이다.
  Gradle은 `UP-TO-DATE`로 test task를 통째로 건너뛰고도 `BUILD SUCCESSFUL` rc=0을 낸다
  (2026-08-27 실증 — 죽은 에이전트가 먼저 돌려 둔 탓에 `kotlin:test`가 아예 미실행인데 rc=0).
  처방: `cleanTest`/`--rerun-tasks`로 강제하고, **집계는 콘솔이 아니라 결과 산출물에서 읽는다**
  (JUnit `TEST-*.xml`의 `tests=`/`failures=`/`errors=`, ctest 요약, node `# pass`/`# fail`).
  "actionable tasks: N executed, M up-to-date" 줄을 반드시 확인한다.
- **잡 로그의 mtime을 본다.** 죽은 잡은 마지막 줄이 그대로 남아 살아 있는 것처럼 보인다
  (실증: jvm 테스트 잡이 13:13에 죽었는데 이후 계속 "게이트 대기 중"으로 오보). 감시에는
  로그 갱신 경과 시간을 함께 찍는다.
- **프로세스 개수는 잡 개수가 아니다.** codex 1잡이 부모/자식 2프로세스로 보인다.
  개수가 아니라 정체(어느 프롬프트인지)를 확인한다.
- 진행 보고는 **출력을 읽은 근거로만** 한다. 프로세스 생존 확인은 진행 확인이 아니다.

## 6. 지표 재측정 명령 (CP2마다 — progress 지표 행 갱신)

```bash
cd framework/languages/dotnet/src/Zlink.Framework
grep -rhoE 'lock *\(' --include=*.cs . | wc -l                        # lock 총계
grep -rhoE 'private readonly object _gate' --include=*.cs . | wc -l    # _gate 보유 클래스
grep -rlE 'ZLinkStateLane' --include=*.cs . | wc -l                    # lane 사용 클래스
grep -rhoE 'AwaitStateLane' --include=*.cs . | wc -l                   # 호환 경계(부채)
```

**cpp** — 2026-08-27 정정. 종전 명령은 `std::lock_guard<`/`(` 형태만 잡아 실제의 1/4만 셌다.
이 트리의 관용구는 `std::lock_guard lock (_mutex);`(변수명+공백)이므로 **`\b`로 끊어야** 한다.

```bash
cd /home/hep7/project/zlink
R=framework/languages/cpp/framework
FILES=$(find $R/src $R/include \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | grep -vE '/tests?/')
echo "$FILES" | xargs grep -hoE 'std::(lock_guard|unique_lock|scoped_lock|shared_lock)\b' | wc -l  # RAII 취득
echo "$FILES" | xargs grep -hoE 'recursive_mutex' | wc -l                                        # 재진입 잔존 신호
echo "$FILES" | xargs grep -lE 'state_lane_t' | wc -l                                            # lane 사용 파일
echo "$FILES" | xargs grep -hoE '\.run\s*\(' | wc -l                                            # lane 호출(브리지 상한)
echo "$FILES" | xargs grep -coE 'std::(lock_guard|unique_lock|shared_lock)\b' | grep -v ':0' \
  | sort -t: -k2 -rn | head -10                                                                  # 상위 파일
```

**java·kotlin**

```bash
cd framework/languages/java
find . -path '*/src/main/java/*' -name '*.java' | xargs grep -hoE '\bsynchronized\b' | wc -l
find . -path '*/src/main/kotlin/*' -name '*.kt' 2>/dev/null | xargs grep -hoE '\bsynchronized\b' | wc -l
```

**node** — lock이 없으므로 `await` 경계 스냅샷이 지표다. 정적 계수는 cp3-audit-node.ko.md §2의
AST 방법을 따른다(단순 grep으로는 재현되지 않는다).

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
- **(9, java 배치4·dotnet 배치19 실측)** **등록·캡처는 반환 전에 완료한다.** 원본이 monitor/lock
  아래서 "호출 반환 시점에 이미 끝나 있던" 등록·판독(waiter 등록+epoch 캡처, store 읽기 완료 등)을
  lane 비동기 게시로 바꾸면 호출자가 완료 전에 진행해 관측 타이밍이 깨진다(NPE·epoch 오귀속
  실측). 처방: 그런 표면은 동기 join(inStateLane/AwaitStateLane)으로 반환-전-완료를 보존하고,
  완료 신호 대기만 비동기로 남긴다. CP1 리뷰 체크리스트: "원본에서 반환 전에 보장되던 완료가
  전환 후에도 반환 전인가".
- **(10, node CP3 이분 실측 — 캠페인이 새로 만든 결함 유형)** **연속된 동기 read가 하나의 파생
  값을 만들 때, 그 read들을 각각 await로 바꾸면 캡처 블록 자체가 찢어진다.** node
  `actor-packet-relay.ts relayRemoteActorPacket`이 실측이다. base에서
  `captureBoundSessionResponseTarget`·`sessionRouteFence`·`committedRoute` 3콜이 전부 동기라
  **한 JS turn에서 원자적으로** 잡혔고 `storedActorRef`는 정합한 스냅샷이었다. 전환이 셋을 모두
  await로 바꾸면서 캡처 블록 **내부에** 경계 2개가 생겼고, `storedActorRef`가 서로 다른 시점의
  값을 섞은 스냅샷이 될 수 있다. 발견 9가 "반환 전에 끝나야 할 완료"라면 이건 그 캡처 측 변형이다.
  처방: N개 read가 한 파생 값을 만들면 그 묶음을 **하나의 turn/lane turn 안에서 함께** 잡거나,
  exact identity로 캡처하고 경계 뒤 재검증한다. read를 하나씩 기계적으로 await로 올리지 않는다.
  **CP1·감사 체크리스트 항목: "원본에서 한 turn에 함께 잡히던 read 묶음이 전환 후에도 함께 잡히는가."**

- **(8, node L2 판정)** node에서 **동기 메서드는 이미 JS turn 원자성으로 lane turn과 동등**하다.
  따라서 동기 공개 계약(d.ts의 sync 시그니처)을 Promise로 바꾸면서까지 lane으로 감싸지 않는다.
  lane 적용 대상은 ① 메서드 내부 await로 상태 읽기·쓰기가 갈라지는 비동기 경로 ② 재진입 검출이
  필요한 public 표면뿐이다. 동기 표면은 무변경으로 두고 근거를 보고에 기록한다.

## 9. 캠페인 종료 후 안건 — 핵심 내부 아키텍처의 언어 간 통일 (2026-08-27 사용자 제기)

**결론: 조건부 찬성이나 지금 형태 그대로는 안 된다. 그리고 이건 구현 통일이 아니라 스펙 판정 사안이다.**

| | dotnet | java |
|---|---|---|
| 클래스 내부 | lane 소유, `RunAsync` 호출 | 동기화 **0** |
| 직렬 보장 주체 | **자기 자신**(lane이 강제) | **상위**(`ZLinkAsyncSerialQueue`) |
| 호환 경계 비용 | **`AwaitStateLane` 664곳** | 없음 |
| 불변식 위반 시 | lane이 예외로 **검출** | **조용히 깨짐** |

java 형태가 더 깨끗하다(브리지 0). dotnet의 664개가 캠페인이 "핫패스 회수는 이월"로 남긴 부채다.

**그러나 안전성이 반대로 간다.** dotnet lane은 자기 강제 — 어느 스레드에서 부르든 직렬화하고
재진입은 예외로 터진다. java 형태는 "호출자가 직렬 큐에서 부른다"는 **검사되지 않는 불변식**에
의존한다. 실증: 이번 세션 jvm 결함 6건 중 pending-session 데드락이 정확히 그 형태였다
(상위 직렬 소유를 전제했다가 재진입으로 깨짐 → 런타임 데드락).

**전환 조건**
1. lane의 자기 강제를 대체할 **소유권 단언**을 함께 넣는다(디버그 빌드에서 "지금 소유 executor
   위인가" 검사). 없이 lane만 걷어내면 안전성 순손실이다.
2. **먼저 664개 브리지가 핫패스에 몇 개인지 측정**한다. 부채 크기가 아니라 비용이 근거다.
3. `ZLinkSpotSerialExecutor`가 이미 actor별·timer별 lane을 갖고 있으니 **진입 경로가 단일인지**
   확인한다(`ZLinkSpotNodeCatalog` 호출자는 4곳이라 검증 가능한 규모).

**정본 문제**: [[reference-first-porting-policy]]상 **.NET이 규칙의 정본**이다. java가 다른 형태로
간 것은 정책 이탈이고, java 쪽이 낫다면 통일 방향은 "dotnet을 java에 맞춘다"가 아니라
**"스펙 06을 고치고 정본을 재선언한다"**여야 한다. 안 그러면 다음 언어 포팅에서 또 갈린다.
따라서 캠페인 종료 후 **별도 스펙 안건**으로 올린다.

### 9.1 실제 쟁점은 동기화 메커니즘이 아니라 **진입 아키텍처**다

cpp `spot_runtime`을 java 방식(클래스 내부 동기화 0)으로 갈 수 있는지 실측했다. **불가능하다.**

| | java `ZLinkSpotRuntime` | cpp `spot_node_runtime_t` |
|---|---|---|
| 진입 파일 수 | 상위 `ZLinkAsyncSerialQueue` 단일 소유 | **8개** |
| 콜백·타이머·dispatch 진입 신호 | — | **217곳** |
| 전용 직렬 실행기 | 있음 | 없음(`spot_route_internal_dispatcher`는 route 전용) |
| 내부 동기화 | `synchronized` 0 | `recursive_mutex` 132곳 |

`recursive_mutex`가 132곳 쓰인다는 것 자체가 **여러 경로에서 재진입해 들어온다**는 증거다.
즉 같은 이름의 컴포넌트인데 **소유·진입 구조가 언어마다 다르게 설계돼 있다.** 동기화 방식은
그 결과일 뿐이다. 따라서 "동기화를 통일한다"는 접근으로는 풀리지 않는다.

### 9.2 통일 안건이 실제로 결정해야 하는 것

1. **핵심 컴포넌트(Spot·Actor·Session)의 진입 소유를 어느 형태로 통일할 것인가**
   - (A) 상위 단일 직렬 소유(java 형) — 내부 동기화 0, 브리지 0. 대신 진입 경로를 하나로
     재설계해야 하고, 자기 강제가 없어 **소유권 단언**이 필수다.
   - (B) 클래스 내부 lane 자기 강제(.NET·cpp 현행) — 어느 경로로 들어와도 안전. 대신
     브리지 부채(dotnet `AwaitStateLane` 664)를 감수한다.
   - (C) 컴포넌트별로 다르게 — 진입이 실제로 단일인 곳만 (A), 나머지는 (B). **현실적 절충안.**
2. **어느 언어가 정본인가.** [[reference-first-porting-policy]]는 .NET 정본이지만, java가
   이미 (A)로 갔다. 정본을 유지하려면 java를 (B)로 되돌리거나, 스펙 06을 고쳐 (A)를 정본으로
   재선언해야 한다. **지금은 양쪽이 공존하며 스펙이 그 사실을 기록하지 않고 있다.**
3. 결정 후 4언어 진입 경로 재정렬 — 이건 이번 캠페인보다 큰 작업이다.

### 9.3 이번 캠페인에서의 처리

**(B)를 유지한다.** cpp는 진입 구조상 (A)가 불가능하고, 정본 정책상으로도 .NET 형태가 맞다.
구현 패스에서 이 선택을 임의로 바꾸지 않는다 — **클래스 내부 lane 없이 "상위가 직렬
보장한다"는 형태로 제출되면 반려**한다.
