# #1673 Java J-S1-1 수정

등급: gpt-6.1-sol medium — registry의 timer 수명과 close 경쟁을 기존 gate 안에서 다루는 코드 작업이다. 사용자 브리프와 감독자의 `review-1673-g.md` 마지막 반려만 처리했다. 시작 branch는 `framework-java/1673-per-message-cost-2`, HEAD는 `a3def38cff`, 작업 트리는 clean이었다.

## 결과와 범위

`ZLinkServiceOperationRegistry` 생성자의 1ms maintenance 예약을 제거했다. 기존 REQUEST 분기의 첫 등록에서만 같은 registry gate 아래 한 번 예약한다. ScheduledFuture가 시작 여부를 나타내므로 별도 flag·lock·cache가 없다. 첫 request가 끝나도 task를 유지하고 close에서 취소한다. REQUEST를 등록한 적 없는 registry는 send 수와 무관하게 예약이 0이다. 사용자 지시의 “첫 REQUEST에서 시작, close에서 취소”를 그대로 따른다.

대안 대조: 첫 REQUEST→close 수명은 시작 한 곳·취소 한 곳이다. 매번 pending request가 0이 될 때 취소하고 재등록 때 재시작하는 대안은 terminal 경로마다 count·취소·재시작 결정을 더하므로 채택하지 않았다.

deadline은 기존처럼 등록 시 `nanoTime + timeoutNanos`이며, 주기·initial delay는 각각 1ms다. 기존 REQUEST 판정을 allocation 앞쪽으로 옮겨 첫 예약 실패가 entry나 dispatcher 자리를 남기지 않게 했다. `expire`의 비교·순서·take·발행은 수정하지 않았다. send의 deadline 필드는 읽히지 않는다. Caller API 변화는 없다.

변경 파일(모두 `framework/languages/java/zlink-framework-core/src/` 아래):

- `main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java`
- `test/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistryTest.java`
- `test/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubmissionTest.java`

## 소유와 판정 위치

소유 계층: Framework runtime의 기존 service operation registry. Binding·Core admission/재제출 결정은 변경하지 않았다.

스펙 조항: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:240` §7(send에 timeout 없음), `:291` §9(request deadline와 terminal 경쟁), `:330` §10(atomic take 단일 소유). Lazy timer 수명 자체는 감독자가 승인한 내부 성능 수정이며 public 계약을 바꾸지 않는다.

변경 분류: B — S1에서 send-only socket마다 기존 request maintenance가 생성되던 성능 결함.

| 결정 | 수정 전 Java | 수정 후 Java / 소유자 |
|---|---|---|
| maintenance 시작 | registry 생성자 :75–76의 무조건 예약 | registry `registerLocked`의 REQUEST 분기 :348–355, 기존 gate 아래 최초 한 번 |
| maintenance 종료 | registry `close` :239 cancel | registry `close` :238, 시작한 task가 있을 때 cancel |
| REQUEST deadline 적용 | `registerLocked` :353 REQUEST 분기 | 동일 메서드 :348–355 REQUEST 분기, 값은 local을 통해 entry로 이전 |
| 만료·terminal 순서 | registry `expire`와 `takeLocked` | 변경 없음 |

수정 전/후 규칙 수(판정 위치): maintenance 시작 1→1, 취소 1→1, REQUEST deadline 적용 1→1, terminal take 1→1. 추가한 자원 참조는 scheduler 하나이며 constructor의 즉시 예약을 제거했다. 시작 여부는 기존 ScheduledFuture의 null 여부로 표현하고 두 번째 상태를 두지 않는다.

교차언어 대조: Kotlin은 Java runtime을 공유한다(`zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:53,66,79`, send caller cancellation을 operation에 전달하지 않음). C++ `framework/src/runtime/foundation/operation_registry.cpp:261`은 deadline을 entry에 보존하고 `:438`의 next_deadline은 조회만 하며 registry 생성자에 주기 task가 없다. .NET `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:45` RegisterSend는 entry만 등록하며 registry 주기 task가 없다; `Runtime/Service/ZLinkManagedMeshNode.cs:9975–9991`은 binding admission을 PendingSend로 관찰한다. 세 언어의 send timeout 없는 사용자 동작은 동일하며 이번 변경은 Java/Kotlin 공유 runtime의 불필요한 timer 예약만 없앤다. C++/.NET 실행 검증은 이 작업 범위에서 하지 않았다.

## #1815와의 정합

읽은 ref: `origin/framework/1646-registry-deadline`, commit `d5184515c1` (로컬 remote-tracking ref). 해당 registry도 `private final ScheduledFuture<?> maintenance`를 생성자에서 `scheduleAtFixedRate(... DEADLINE_TICK_NANOS ... NANOSECONDS)`로 예약한다. **#1815는 아직 lazy가 아니다.**

이번 수정은 생성자·필드·close·registerLocked에 국한된다. #1815의 deadline bitmap/rounds/cursor/indexDeadline/expire 코드는 건드리지 않으므로 wheel 알고리즘과 새 상태를 결합하지 않는다. 두 브랜치가 같은 registry를 고치므로 hunk 충돌 가능성은 있으며, 기존 #1673의 pool/Kind/takeLocked 대 wheel의 구조 차이가 주된 병합 범위다. 이번 수정으로 추가된 병합 의무는 scheduler 저장, REQUEST 최초 예약, nullable future cancel 세 가지다. #1815의 1ms NANOSECONDS 상수와 deadlineOrigin/tick 기준은 유지하고, send를 wheel에 넣지 않도록 Kind 분기를 보존하면 된다. 적용 완료나 무충돌을 주장하지 않는다.

특히 #1815는 constructor에서 deadlineOrigin을 설정하고 maintenance가 deadlineTick을 전진시킨다. lazy 적용 뒤에는 오래 idle이었던 registry의 첫 REQUEST에서 wheel이 과거 tick을 budget 안에서 따라가는 경로를 확인해야 한다. 이번 브랜치에는 wheel이 없으며 이 보고는 병합 시 검증할 경계를 명시한 것이다. 실제 merge와 hunk 재리뷰는 감독자 범위다.

## 시험과 검증

로그 위치: `/root/jobs/fx/cx/m1673k/`.

- 수정 전: 새 시험 3개만 실행해 **3 tests / 3 failures**, `red.log`, `red-registry.xml`, `red-submission.xml`. 두 registry 시험은 생성 직후 예약 0 기대에 실제 1로 실패했고, socket 생성 시험은 Dealer의 maintenance future가 null이 아니어서 실패했다. 런타임 수정 전에 실행한 결과다.
- 가짜 ScheduledExecutorService는 JDK Proxy로 scheduleAtFixedRate 호출 수·인자와 cancel(false)를 기록한다. 실제 timer thread·sleep 없이 callback을 가상 clock에서 실행한다. send-only registry의 생성·pending send·send 완료·close 동안 예약 0, 첫 REQUEST 뒤 예약 1 유지, 만료 직전/정각, close 취소와 close 뒤 등록 거부를 확인한다.
- socket 생성 시험은 Dealer/Router/독립 RawServicePort가 실제로 만든 registry의 maintenance field가 null인지 확인한다. 런타임에 시험용 scheduler 주입 API를 추가하지 않았다.

- 수정 후 focused: `:zlink-framework-core:test --tests '*ZLinkServiceOperationRegistryTest' --tests '*ZLinkJavaSubmissionTest'`, **68 tests / 0 failures / 0 errors / 0 skipped**. Registry 31개, submission 37개. `focused.log` BUILD SUCCESSFUL, `focused-summary.txt`에 XML 집계 보존.

- 전체 명령: `cd framework/languages/java && env -u ZLINK_LIBRARY_PATH ./gradlew --no-daemon --max-workers=2 --continue --rerun-tasks -Pzlink.includeSamples=false test contractTest`.
- `full.log`: exit 0, **BUILD SUCCESSFUL in 7m 45s**, **65 actionable tasks: 65 executed**. Unit **3207/0/0/0**, contract **91/0/0/0**(tests/failures/errors/skipped). Core unit 2744, Kotlin unit 82, stream unit 202. XML 모듈별 집계는 `full-summary.txt`. 새 실패 0. Integration/fakeBackend 전용 실행은 하지 않았다.
- `scripts/format/format.sh --check java`: exit 0, `format-check.log`. `git diff --check`: 통과.
- Java API snapshot: exit 0, `api-java.log`; sha256 `c0fef6f02d502620fd0b84559f2be876c361aef5d48d0e8250c537d2963e86f3`, 2968줄. 기존 snapshot과 동일.
- 전체 시험의 asset build가 생성한 Unity `.jspre`는 이번 실행 전 clean HEAD의 bytes로 정리했다. 복원 전후 SHA-256은 `generated-cleanup.txt`. 다른 프로세스를 종료하지 않았다.

- Kotlin API snapshot: exit 0, `api-kotlin.log`; sha256 `6b91bc6d4b4ebf2cccf24cd3243d81a046a4a5a9df941455a1647f363b98808c`, 3590줄. 기존 snapshot과 동일.
- 커밋: `57fc2ecc45` — `java: start operation maintenance on the first request` (3 files, +127/−6). 본문 마지막 `Refs #1673`. Push 없음. Project 보드 변경 없음.
- 최종 작업 트리 clean. 남은 Java 시험 실패 0. #1815와의 실제 병합·wheel idle 경로 검증은 이번 작업에서 실행하지 않았으며 감독자에게 위 정합 메모로 전달한다.

## 자체 점검

성능: send-only registry당 1kHz task 생성 제거. Request당 task나 clock 조회는 추가하지 않았다.
POSDDD·제어 소유: 시작·취소를 기존 registry에 유지하고 기존 gate를 사용한다. REQUEST 판정 분기를 복제하지 않았다.
불필요 코드·상태: 무조건 생성자 예약 삭제, 새 helper 계층·lock·queue·cache·started flag 없음.
리팩토링 점검: 성능 1건·POSDDD 0건·불필요 코드 0건 발견, 성능 결함 1건 수정, 신규 잔여 발견 0건. #1815 병합 시 wheel idle 경로 확인만 감독자에게 전달한다.

Project 보드·스펙·다른 branch·다른 프로세스를 변경하지 않았다. Push하지 않는다.
