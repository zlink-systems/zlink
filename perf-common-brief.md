## 공통 원칙 (0.31.0 메시지당 비용 감축 job, 네 언어 공통 — 이 절은 모든 job에서 같은 문장으로 유지한다)
목표: framework 0.31.0의 처리량을 binding 대비 0.90 이상으로 올린다(framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md §7(c)). 감사·진단 결과로 binding과의 차이는 메시지당 비용의 차이이므로, 이 job은 메시지당 비용을 줄인다. 메시지당 예산(send target 기준): CPU ≤ binding×1.1, 할당 ≤ 4, mutex/monitor ≤ 3, thread 전환 ≤ 0.1. 메시지당 허용 작업 표(#1639 10-13 댓글): send = 미리 준비한 target·header로 encode 1회 + binding submit 1회; receive = 회전 단위 수신, header 해석 1회, payload 소유권 이전, 같은 owner turn에서 handler 호출; reply·completion = binding reply 1회, turn당 묶은 completion 전달; trace off = 비용 없음. 금지: 메시지별 lane 합류·snapshot·DTO·task/Promise/future·permit 예약/반환·thread 전환·lock.

네 언어(C++·Java/Kotlin·.NET·Node)는 같은 시스템이다. 같은 스펙 조항(08-messaging-hot-path, 02-handler-turn-and-execution-gate, 06-state-ownership-and-lanes, submit·completion 스펙)을 기준으로 하고, 한 언어만 다르게 동작하게 만드는 수정·언어별 예외를 넣지 않는다. 수정마다 다른 세 언어가 같은 결정을 어디서 내리는지(file:line) 대조해 보고한다. 비용을 줄이려면 스펙이 정한 turn·전환·계약을 바꿔야 하는 경우, job은 스펙을 고치지 않고 그 항목을 "스펙 개정 필요"로 file:line·근거·제안 문안과 함께 보고한다 — 감독자가 네 언어 공통으로 한 번 개정하고(규칙이 줄어드는 방향, codex sol 리뷰) 이어서 지시한다. 그 항목에서 멈추되 다른 항목은 계속한다.

문체는 doc/principal/documentation/documentation-principles.ko.md 원칙 7. 다른 agent·codex job을 띄우지 않는다. 스펙(framework/doc/framework/common/{spec,e2e,sample,internals}, core/doc/spec, bindings/doc/spec)은 고치지 않는다. AGENTS.md §3을 따른다: 결정은 소유 계층 한 곳, 금지 패턴(timeout·budget·retry 증가, 안전을 위한 재시도·순서 변경, generation·flag로 걸러내기, 두 번째 poller, catch-all, fixture 완화, 구현에 맞춘 assertion 변경), 새 lock·queue·timer·retry·pool·cache 금지, execution gate를 우회하는 동기화 금지, 결과 설계의 판정 위치 수는 줄거나 같다.

작업 위치: 브리프 끝 "이번 job" 절의 worktree·브랜치·언어 디렉터리만. 커밋은 그 브랜치에만, `git add <경로>`(-A 금지), 본문에 `Refs #<issue>`, 끝에 빈 줄 뒤 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. push·PR·merge(아래 main merge 지시 제외)·브랜치 전환·reset·stash·rebase, GitHub(gh)·보드·Issue, pkill·killall(PID로 종료) 금지. 다른 worktree는 읽기만. 로그·임시 파일은 /root/jobs/fx/cx/<job>/ 아래에만.
빌드 준비: worktree에서 `source ~/.bashrc && bash scripts/gate/rebuild-dev.sh <job>`(Core·로컬 패키지). main 기준선 비교용 worktree: /root/worktree/zlink-main-base(읽기·빌드만).
출력 제한: 빌드·테스트 출력은 로그 파일로, 첫 실패·관련 구간·마지막 요약만 읽는다. 파일은 필요한 줄만 읽고 다시 읽지 않는다.

측정 규칙: 이 job은 **시간 기반 성능 측정(처리량·latency·CPU µs)을 하지 않는다** — 다른 job이 같은 머신에서 빌드·시험 중이라 오염된다. 효과 확인은 부하에 영향받지 않는 횟수로 한다: 메시지당 할당 수(바이트), lock/monitor 획득 수, thread 전환(실행 thread가 바뀐 횟수), task·Promise·future 생성 수, 단계별(E1–E5, I0–I4, W1–W4) 수. 계측 방법은 이전 보고서(/root/jobs/fx/cx/의 m1639d2·m1639n3·q1673d·q1673e·m1672b 보고서)의 방법을 재사용하고, 계측 코드는 커밋하지 않는다(필요하면 별도 사본·패치로). 수정 전과 후를 같은 방법으로 세어 표로 낸다. 시간 측정은 감독자가 묶음이 끝난 뒤 단독으로 한다.

진행 단계:
1. **계획(여기서 멈춤)**: 이전 감사·진단 보고서와 #1639 댓글의 비용 항목 중 이 언어 것을 모두 모아, 현재 브랜치 HEAD에서 아직 남은 항목을 file:line으로 확인한다. 항목마다: 메시지당 비용(횟수), 위 허용 작업 표 위반 여부, 소유 스펙 조항, 다른 세 언어의 대응 위치, 수정안, 분류(A/B/C/D), "스펙 개정 필요" 여부. 비용이 큰 순서로 정렬한다. 보고서 /root/jobs/fx/cx/<job>-plan.md를 쓰고 최종 응답으로 요약(20줄 이내)을 보낸 뒤 멈춘다. 감독자가 승인 항목을 지시한다.
2. **구현(승인 뒤)**: 승인된 항목을 큰 것부터 하나씩 — 수정 전 횟수 → 수정 → 수정 후 횟수 → 관련 시험. 항목마다 커밋 하나. 끝에 그 언어 단위·contract 전체 시험과 format 검사, main 대비 새 실패 0.
3. **완료 보고** /root/jobs/fx/cx/<job>-report.md: 항목별 전/후 횟수 표, 변경 파일, 실행한 시험과 결과, 남은 실패, 소유 계층·spec 조항·교차언어 대조·분류 각 한 줄, 수정 전/후 판정 위치 수, 커밋 sha, 남은 예산 초과분(file:line), "스펙 개정 필요" 목록, /root/jobs/fx/finish-refactor.md 형식의 리팩토링 점검 한 줄.
