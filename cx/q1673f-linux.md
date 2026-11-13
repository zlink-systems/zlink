등급: opus — 측정 기반 진단과 실행 경계 수정이라 판단이 필요하다. (원 브리프 D:/t/jobs/fx/cx/q1673f.md를 이 Linux 성능 머신에 맞게 고친 것)

## 공통 원칙
문체 기준은 /root/zlink/doc/principal/documentation/documentation-principles.ko.md 원칙 7이다. 다른 agent·codex job을 띄우지 않는다. 스펙(framework/doc/framework/common/{spec,e2e,sample,internals}, core/doc/spec, bindings/doc/spec)은 고치지 않는다 — 구현이 스펙과 다르면 그것은 고칠 결함(B)이다. 스펙이 정하지 않은 결정, 스펙끼리의 충돌, 스펙을 바꿔야만 풀리는 문제만 BLOCKERS로 file:line과 함께 보고하고 그 부분은 멈춘다.

AGENTS.md §3을 따른다: 결정은 소유 계층에서 한 곳이 내린다. 금지 패턴(timeout·budget·retry 증가, "안전을 위한" 재시도·순서 변경, 별도 generation·flag 상태로 걸러내기, 같은 socket에 두 번째 poller, catch-all, fixture 완화, 구현에 맞춘 assertion 변경)을 쓰지 않는다. 새 lock·queue·timer·retry·pool·cache를 더하지 않는다. Execution gate(framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md)를 우회하는 동기화를 남기지 않는다. 결과 설계의 판정 위치 수가 줄어야 한다.

작업 위치: worktree /root/worktree/zlink-1639-msg-java, 브랜치 framework-java/1639-per-message-cost(HEAD a2e5bbce5f = 4458abe381 + main 4bf652482f merge + 컴파일 수정). Java·Kotlin 디렉터리(framework/languages/java)만 고친다. 커밋은 이 브랜치에만, 파일은 `git add <경로>`(`git add -A` 금지), 본문에 `Refs #1673`, 끝에 빈 줄 뒤 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. push·PR·merge·브랜치 전환·reset·stash·rebase 금지. GitHub(gh)·Project 보드·Issue 금지. pkill·killall 금지(PID로 종료).
측정용 변형 빌드가 필요하면 `git worktree add --detach /root/worktree/q1673f-<이름> <commit>`으로 별도 worktree를 만들고(그 안에서만 패치·빌드), 끝나면 `git worktree remove`로 지운다. 모든 로그·harness·임시 파일은 /root/jobs/fx/cx/q1673f/ 아래에만 둔다.
빌드 준비: 각 worktree에서 `source ~/.bashrc && bash scripts/gate/rebuild-dev.sh <tag>`가 Core·로컬 패키지를 만든다(zlink-work/gates/<tag>/ 로그). Java 시험은 /root/jobs/fx/cx/g1673m/run-tests.sh <checkout> <tag> 형식을 참고한다.
출력 제한: 빌드·테스트 출력은 로그 파일로 보내고 첫 실패·관련 구간·마지막 요약만 읽는다. 파일은 필요한 줄만 읽고 다시 읽지 않는다.
절차: (1) 수정 전 실패(여기서는 CPU 수락 조건 미달)를 측정으로 먼저 보인다. (2) 수정. (3) 관련 테스트 → Java/Kotlin 전체 label 한 번과 format 검사. main 대비 새 실패가 없음을 확인한다(main 기준선: /root/jobs/fx/cx/g1673m/main/summary.txt — 3131 tests, 0 failed; format 기존 위반 2건 ZLinkJsonMessageSerializerTest.java, KotlinMessageCodecBoundaryTest.kt는 main에도 있음).

## 측정 규칙 (필수)
- 측정은 단독이다. **측정은 /root/jobs/fx/cx/g1672m-report.md 파일이 생긴 뒤에만 시작한다**(그 전에는 다른 job이 C++ 빌드·시험을 돌린다). 매 셀 시작 전 /proc/loadavg 1분 값 < 5를 확인하고, 측정 중에는 이 job도 빌드·테스트를 돌리지 않는다. 셀마다 시작·끝 load와 동시에 돌던 무거운 프로세스(ps, CPU 상위 5개)를 기록하고, 간섭이 있었던 셀은 수치를 보지 않고 invalid로 표시해 다시 잰다.
- `flock /tmp/zlink-1639-measure.lock`으로 측정을 감싼다.
- 1024 B. Java CPU 진단은 같은 JVM에서 warmup 30초 뒤 active 5초(2초 warmup은 C2 JIT가 섞여 오측정한다). 변형들을 교대로(reference → before → 묶음 a → b → c → after …) 3회 돌려 중앙값을 쓴다.
- 판정 값: serial(request-serial)·send(send-saturation)의 source·target 프로세스 CPU µs/msg(프로세스 CPU 시간 ÷ 메시지 수), 처리량, 같은 binding 셀 대비 비율. 모든 셀에서 received/completed 일치·오류 0을 확인한다. JFR(CPU 표본, jdk.ThreadPark, 필요하면 -XX:-UseTLAB + ObjectAllocationOutsideTLAB)과 `perf stat -e context-switches`(또는 /proc/<pid>/status의 voluntary_ctxt_switches)는 CPU 측정과 별도 실행으로 수집한다.
- harness: 이전 harness(q1673e/evidence.tar.gz)는 이 머신에 없다. 저장소의 framework messaging bench(framework/doc/framework/common/bench/with-grpc-local.ko.md와 그 runner, framework/languages/java/perf 등)에서 Java framework·binding의 request-serial·send-saturation 셀을 실행하는 경로를 찾아 쓰고, 위 측정 규칙(30초 warmup, CPU µs/msg 수집)을 입히는 얇은 스크립트를 /root/jobs/fx/cx/q1673f/harness/에 만든다. 저장소 코드는 고치지 않는다. 이전 수치(175.9/263.7µs 등)는 다른 머신 값이라 비교 대상이 아니다 — reference·before·after를 모두 이 머신에서 다시 잰다. harness를 어떻게 만들었는지 보고서에 적는다.

## 이번 job (q1673f, #1673 Java CPU 회귀 원인)
- 배경 보고서: /root/jobs/fx/cx/q1673e-report.md, q1673d-report.md (BLOCKERS B1·B2와 수정 경위).
- 문제: 4458abe381은 할당을 줄였지만 이전 머신 측정에서 serial CPU가 before 175.9/169.0µs → 263.7/217.7µs(source/target)로 늘었다. send source도 24.8→27.6µs.
- 커밋 기준: reference = 848ce30597, before = 01d3ed75e7(4458abe381의 부모), after = 4458abe381. merge 뒤 현재 HEAD = a2e5bbce5f.
- 할 일:
  1. 4458abe381의 변경을 세 묶음으로 나눈다: (a) B1 worker 제출 일원화(ZLinkSerialExecutionQueue), (b) B2 durable terminal/caller 분리와 StateLane common-pool 게시(ZLinkJavaDurableRequest·ZLinkStateLane), (c) reply wrapper 제거·UTF-8 scratch 제거. before에 각 묶음만 얹은 빌드를 만들어(별도 worktree) 같은 harness로 serial·send CPU를 잰다. reference·before·after도 함께 잰다. 회귀를 만든 묶음과 위치를 file:line으로 확정한다(측정과 JFR CPU 표본·thread park/unpark 수·context switch 수로). 먼저 이 머신에서 회귀가 재현되는지(after > before)부터 보이고, 재현되지 않으면 그 사실과 수치를 보고하고 2로 넘어가지 말고 멈춘다.
  2. 진단 결과를 보고서에 먼저 쓴다: 결정 소유 표(원인 결정마다 스펙 소유 조항, 지금 그 결정을 내리는 곳 — Java file:line과 .NET·C++ 대응 위치, 하나로 모을 소유자), 변경 분류(A/B/C/D), 수정안. **여기서 멈추고 감독자 승인을 기다린다**(최종 응답으로 진단 요약을 보내고 끝낸다. 감독자가 이어서 지시한다).
  3. (승인 뒤) 원인 묶음을 스펙을 지키며 고친다. B1·B2는 결함 수정이므로 되돌리지 않는다(02-handler-turn-and-execution-gate §2, 08-messaging-hot-path §4.1 I4·§3 E5, 06-state-ownership-and-lanes 「완료 신호와 블로킹 호환 경계」). 예: caller 게시가 idle common-pool thread를 깨우는 비용이면 이미 실행 중인 소유자(caller의 실행 문맥·기존 executor)로 게시하는 경로를 .NET(StateLane.cs:114/160)·C++(task.hpp:346)와 대조해 고른다.
  4. 수락 조건(이 머신 측정): 수정 후 HEAD의 serial source·target CPU ≤ reference(848ce30), send source·target CPU ≤ before. B1·B2 회귀 시험(ZLinkJavaDurableRequestOwnerTest 등)은 계속 통과.
- 검증: Java/Kotlin `test contractTest fakeBackendTest integrationTest` 전체, format·API snapshot, main 대비 새 실패 0. main STREAM 간헐(ZLinkStreamActorRegistryTest:201, ZLinkStreamLifecycleRaceTest:194)은 이 job이 고치지 않는다(나오면 기록만).

## 출력
보고서 /root/jobs/fx/cx/q1673f-report.md: 결론 먼저, harness 설명, 변형별 CPU·처리량 표(3회 값과 중앙값, invalid 셀 표시), 원인 file:line과 근거(JFR·park·ctx), 결정 소유 표, 분류, 수정안. 구현 단계까지 갔다면 추가로 변경 파일, 시험 결과, 소유 계층·spec 조항·교차언어 대조·분류 각 한 줄, 수정 전/후 판정 위치 수, 커밋 sha, 그리고 /root/jobs/fx/finish-refactor.md 형식의 리팩토링 점검 한 줄.
