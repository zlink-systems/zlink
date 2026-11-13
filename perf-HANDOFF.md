## 0.31.0 성능 작업 인계 (2026-10-15, 다른 머신 이관용)

push한 브랜치(검토 전 WIP 포함, main 머지는 0.30.1 태그 뒤):
- `framework-cpp/1639-per-message-cost` (#1672, 마지막 커밋은 미검토 WIP, Core POLLIN events 결함으로 막힘)
- `framework-java/1639-per-message-cost` (#1673)
- `framework-cpp/1645-serial-roundtrip` (#1645)
- `framework/1648-receive-path` (#1648)
- `framework/1646-registry-deadline` (#1646)

인계 뒤 바뀐 전제: #1789(C++ Yield turn 반납) 머지로 Yield 1회당 비경합 turn mutex가 4→5회가 됐다(#1645 측정에 반영). 0.30.1 수정(#1712·#1745·#1761·#1769)이 수신 경로·permit·수신 gate를 바꾸므로 #1640 baseline은 0.30.1 태그 뒤 다시 잰다.

---

# 0.31.0 성능 개선 인계 (2026-10-14, zlink-cf → 성능 전담 세션)


## 0. 2026-10-14 밤 갱신 — 먼저 읽을 것 (이 절이 아래 내용보다 우선한다)

**작업 시작: 2026-10-15 오전, 새 성능 전용 머신에서.** 이 문서의 §2·§5는 10-14 오전 기준이다. 바뀐 점은 다음과 같다.

### 0.1 머지 규칙 (A안, 실장님께 추천·기본값)
- 0.30.1은 main에서 바로 태그를 자른다(10/16 오후 예상). 그래서 **0.30.1 태그 전에는 성능 PR을 main에 머지하지 않는다.** 머지하면 0.30.1에 실려 범위가 바뀌고 c2 검증이 흐려진다.
- 성능 작업은 지금 시작해 브랜치에서 진행하고, **하루 한 번 이상 main으로 rebase**한다.
- 0.30.1 태그 직후부터는 리뷰를 통과한 PR을 바로바로 main에 머지한다(통합 브랜치에 쌓지 않는다).
- 실장님이 B안(릴리스 브랜치를 잘라 main을 연다)을 고르시면 이 절을 바꾼다. 0.30.1 진행은 zlink-cf 세션이 맡는다.

### 0.2 바로 시작해도 되는 것 / 기다릴 것
| 바로 시작 | 이유 |
|---|---|
| Core `ZLINK_INTERNAL_OPT_EVENTS`(C++ #1672 막힘) | 0.30.1은 Core를 건드리지 않는다. Core 1.20·binding 게시는 실장님 확인 필요 |
| Node #1675 측정·개선 | 0.30.1의 Node 변경은 #1712 relocation 일부뿐 |
| Java #1673 q1673f(serial CPU 회귀 가르기) | **먼저 main으로 rebase.** #1743(PR #1759·#1764)이 `ZLinkSerialExecutionQueue.java`·`ZLinkCompletionBridge.java`·`ZLinkRequestStages.java`를 바꿨다 |
| #1640 측정 기준선 준비(스크립트·셀 정의) | 코드 충돌 없음 |

| 기다릴 것 | 겹치는 0.30.1 수정 | 시점 |
|---|---|---|
| .NET #1674 | #1769(`ZLinkRuntimeTaskRunner.RunLongRunning`을 동기 루프 전용으로, ClientServer·Stream·Channel 수신 루프), #1712(relocation, `ZLinkManagedMeshNode.cs`) | 두 PR 머지 뒤 |
| C++ hand-off §4.4, #1645 serial | #1745 C++ Yield gate 재개 경로(`task.hpp`, gate 반납을 suspend 시점으로) | 그 PR 머지 뒤. **실장님 결정: Yield는 0.30.1에서 결함만 고치고 §4.4 hand-off는 0.31.0에서 이 구조 위에 올린다** |
| 언어별 최종 기준선 | 0.30.1이 수치를 바꾼다(#1723 local send 비용 증가 → #1674에서 줄이기로 함, #1769, C++ Yield) | 0.30.1 머지 완료된 main 기준 |

### 0.3 오늘 바뀐 작업 규칙
- **sub-agent는 Claude agent만** 쓴다(Agent 도구, 판단·동시성·진단은 opus, 변경 지점이 분명하면 sonnet-coder). codex는 새로 넣지 않는다. **리뷰는 codex `gpt-6.1-sol`**(스펙 diff). 구현 diff는 감독자가 직접 전부 읽는다.
- `doc/principal/dev/supervisor-guide.ko.md`를 그대로 따른다. 특히 §3: **설계는 감독자가 정해 브리프에 적는다**, **진단과 구현을 나눈다**(결정 소유 표 → 감독자 A·B 승인 → 구현). §5: 1~2회 통과는 근거가 아니다(부하 반복). §6 10-14 사례 3줄을 읽는다.
- 머지 전: 수정 전 실패가 **동작으로** 재현돼야 한다(컴파일 오류는 재현이 아니다). 동시성·실행 스레드 보장은 런타임 동작(JDK CompletableFuture 등)과 대조한다.
- C++ PR은 `gh workflow run framework-cpp-prebuilt-check.yml --ref <branch> -f platform=all`이 통과한 뒤 머지한다.
- agent에게 `pkill`·`killall` 금지, stress-ng는 `--timeout`을 필요한 만큼만 주고 PID로 종료하게 적는다. load 기준(15)은 새 job 투입에만 쓰고, 실행 중 단계마다 기다리게 하지 않는다. 성능 측정만 load < 5 단독이다.
- WSL 명령은 Git Bash에서 스크립트 파일로(`wsl.exe -e bash <파일>`) 실행한다.

### 0.4 0.30.1 상태(10-14 밤) — 참고
머지 완료: #1723, #1729(metadata), #1743(Java·.NET·C++ 및 후속 #1764·#1766), #1745 일부(Java 2, .NET 2, C++ 1). 진행 중: #1712 재작업, #1716·#1738 재작업, #1769, #1745(C++ Yield·취소, .NET 간헐 2). 0.30.2로 넘김: #1761(cross-node Join connection 교체, 스펙 gap), #1763(Java admission view와 JDK 결합자), #1767·#1771·#1772(템플릿 보고), #1770(완료 스레드 재개 스펙 gap).

## 1. 범위와 목표

- 릴리스: framework **0.31.0은 성능 개선만** 다룬다. 성능 외 결함은 0.32.0(또는 진행 중인 0.30.1 패치)로 보낸다.
- 에픽 **#1639**와 하위 이슈:
  - 측정: #1640(언어·패턴별 바인딩 대비 기준선). 이전 측정 근거는 #1390.
  - 공통 원인: #1641(메시지마다 Location owner 조회), #1642(완료를 별도 executor에서 동기 대기), #1643(envelope header를 JSON으로 파싱·할당), #1644(ClientServer stream 95%), #1645(request-serial 왕복 지연), #1648(claim→handler 경로의 메시지별 재구성).
  - 언어별 메시지당 비용: #1672 C++, #1673 Java, #1674 .NET, #1675 Node.
  - 닫힘: #1613, #1638, #1676.
- 바인딩 대비 목표(사용자 결정):
  - spec `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md` §7(c) ≥ 0.90.
  - ClientServer stream 95%, RouteMesh send 90%, request/response continuous 85%, serial 90%.
  - 1차 완료 기준: Java·Node의 오버헤드가 최소한 .NET 비율 수준이어야 한다(같은 구조이므로).
- 사용자에게 이미 보고한 전망: send 90%·serial 90%는 어렵고 현실적으로 60–75%. 이 판단은 조용한 머신 단독 측정 뒤 확정해서 보고한다.

## 2. 현재 상태(10-14 11:00)

| 언어 | 이슈 | 상태 | 근거 |
|---|---|---|---|
| .NET | #1674 | main 머지(PR #1696, #1706). 최근 측정 send 0.549·serial 0.565·backpressure 0.471(바인딩 대비) | `D:/t/jobs/fx/cx/m1639d2-report.md` |
| Node | #1675 | main 머지(handler 활성화 Promise 감축, reply Buffer 유지, payload 유지 정책 일원화). 측정 필요 | `m1675b-report.md`, `m1639n3-report.md` |
| Java | #1673 | 브랜치 `framework-java/1639-per-message-cost`(worktree `D:/worktree/zlink-1639-msg-java`, HEAD 4458abe381). B1·B2 결함 수정과 할당 감축은 했지만 **serial CPU가 176→264µs로 회귀**. 원인 가르기 job 브리프 준비됨: `D:/t/jobs/fx/cx/q1673f.md`(아직 미투입) | `q1673e-report.md`, `q1673d-report.md` |
| C++ | #1672 | 브랜치 `framework-cpp/1639-per-message-cost`(worktree `D:/worktree/zlink-1639-msg-cpp`). **Core 결함으로 막힘**: `core/src/runtime/sockets/common/socket_base_api.cpp:777–786`의 `ZLINK_INTERNAL_OPT_EVENTS`가 POLLIN을 보고하지 않아 binding `events()` getter가 수신 도착을 관찰할 수 없다. Core 수정은 0.31.0 주기에 하기로 사용자 승인됨, 아직 미착수. Core 1.20 + binding 릴리스 필요(**태그·게시는 사용자 확인 필요**) | `m1672b-report.md` |

스펙:
- request-serial 스펙 변경 ① 확정·머지.
- receive-role hand-off 스펙 변경 ② 확정·머지(PR #1698, 738264ecb0). **§4.4 구현은 C++·Java가 남음.** 설계 진단은 `q1645r-report.md`(현재 구조 유지 추천과 ②의 조건).

그 밖의 worktree: `D:/worktree/zlink-1639-fw-perf`(framework/1639-fw-throughput), `D:/worktree/zlink-1639-liveness-test`(framework-cpp/1639-liveness-test-collector). 이어서 쓸지 정리할지는 브랜치 상태를 보고 정한다.

## 3. 다음 순서(추천)

1. **Java q1673f** 투입(단독 측정): 4458abe381을 세 묶음(B1 worker 제출 / B2 durable caller 게시 / reply·UTF-8)으로 나눠 serial·send CPU를 따로 재고, 회귀 위치를 고친다. B1·B2 결함 수정은 되돌리지 않는다.
2. **Core EVENTS 수정**: Core 수정 규칙(동기화 모델·hot path, hotpath_gate 전체 셀 전후 비교, TSan 전후)을 지킨다. Core 1.20·binding 릴리스는 사용자 확인 뒤.
3. C++ #1672 이어서(Core 1.20 local package로 검증).
4. hand-off §4.4 C++·Java 구현.
5. 네 언어를 하나씩 조용한 머신에서 측정해 언어·패턴별 바인딩 대비 비율 표와 목표 달성 가능성을 사용자에게 보고(#1640).

## 4. 측정 규칙

- 성능 측정은 **단독**이다. 측정 중에는 같은 머신에 빌드·테스트를 넣지 않는다. 시작 전 WSL `/proc/loadavg` 1분 값 < 5를 확인한다.
- framework·bindings perf: warmup 2초, 측정 5초, 3회 중앙값, 언어 하나씩 순서대로, 빌드 1회.
- **Java CPU 진단은 같은 JVM warmup 30초 뒤 active 5초**(2초 warmup은 C2 JIT가 섞여 417 vs 176µs로 오측정했다).
- 교대 측정(기준·수정 전·수정 후), `flock /tmp/zlink-1639-measure.lock`. 판정은 처리량 비율 + CPU µs/msg + 할당 byte/msg.
- 동작 확인이나 diff 리뷰만으로 성능 개선을 판정하지 않는다. 스펙의 측정 조건(§7(c) 0.90, §3 전환 횟수)은 실제로 돌린다.

## 5. 머신 공유 — 0.30.1 패치 세션(zlink-cf)과 조율

- zlink-cf는 0.30.1 결함(#1699, #1712, #1716, #1717, #1718)을 codex job 여러 개로 처리 중이다. 오늘 밤 릴리스 예정.
- **측정을 시작하기 전에 zlink-cf에 SendMessage로 알린다**(측정 시작·예상 종료). zlink-cf는 그 사이 새 job을 넣지 않는다. 이미 돌고 있는 job이 load를 올리면 측정을 미루거나 그 셀을 무효로 표시한다(수치를 보고 셀을 고르지 않는다).
- 측정이 아닌 빌드·구현 job은 load < 15(20코어의 3/4)일 때 새로 넣는다.
- codex job 이름은 성능 쪽 접두사(`m16xx`, `q16xx`, `p16xx`)를 쓰고 0.30.1 job(`r1699`, `x1712b`, `i1716`, `f1717`, `f1718` 등)과 겹치지 않게 한다. 같은 파일을 두 세션이 동시에 고치지 않는다 — 특히 Java `ZLinkSerialExecutionQueue.java`는 #1699(r1699)도 고친다. 0.30.1이 main에 머지된 뒤 rebase한다.

## 6. 작업 도구

- codex 실행: `bash /d/t/jobs/fx/cx/launch.sh <job> <worktree>` (`MODEL=`/`EFFORT=` env, 기본 gpt-6.1-sol medium). 백그라운드 실행 timeout 7200000.
- 이어서: `resume.sh <job> <model> "<msg>"`. 세션 입력이 50만 토큰을 넘으면 resume하지 말고 새 세션으로 브리프를 다시 쓴다.
- 감시: `bash /d/t/jobs/fx/cx/wait-any.sh <jobs...>`(백그라운드, -last.txt 갱신 또는 30분 idle에서 반환). load 대기 후 투입: `launch-when-quiet.sh <job> <worktree>`.
- 브리프는 python으로 만든다: `p1645.md` 3–22줄(공통 원칙) + `<job>.job.md` + `finish-refactor.md`. 브리프에 GitHub·보드·Issue 금지, push 금지를 적는다.
- 보드: `bash /d/t/jobs/fx/cx/board.sh <issue> <Todo|InProgress|Review|Done>`(이슈가 보드에 없으면 먼저 `gh project item-add 1 --owner zlink-systems --url <url>`).
- GitHub GraphQL 한도에 걸리면 PR 생성·머지는 REST `gh api`로.

---

### perf-PROMPT.md

zlink framework 0.31.0 성능 개선(에픽 https://github.com/zlink-systems/zlink/issues/1639)을 이 세션이 전담한다.

먼저 다음을 읽어:
1. D:/project/zlink/AGENTS.md, D:/project/zlink/CONTRIBUTING.ko.md
2. D:/t/jobs/fx/perf-HANDOFF.md — 현재 상태, 언어별 브랜치·worktree, 다음 순서, 측정 규칙, 머신 공유 규칙, 작업 도구가 다 있다.
3. #1639의 마지막 댓글(10-14 진행 상황)

진행 방식:
- sub-agent는 Claude agent만 쓴다(판단·동시성·진단은 opus, 변경 지점이 분명하면 sonnet-coder). 리뷰는 codex gpt-6.1-sol(스펙 diff)이고, 구현 diff는 감독자(너)가 직접 전부 읽는다. 스펙 수정은 감독자가 직접 하고 sol 읽기 전용 리뷰를 거친다. 인계 문서 §0(10-14 밤 갱신)이 그 아래 내용보다 우선한다.
- 모든 diff는 테스트까지 직접 리뷰하고, 문제가 없으면 묻지 않고 PR을 머지한다. 태그·릴리스(Core 1.20·binding 포함)는 내게 확인받는다.
- 성능 판정은 실제 측정으로만 한다. 측정은 단독으로 하고, 시작 전에 0.30.1 패치 세션(zlink-cf)에 SendMessage로 시작·예상 종료를 알린다. zlink-cf는 그동안 새 job을 넣지 않는다.
- 0.30.1 패치 세션이 고치는 파일(특히 Java ZLinkSerialExecutionQueue.java)을 동시에 고치지 않는다. 0.30.1이 main에 머지되면 rebase한다.
- 첫 작업은 HANDOFF §3의 1번(Java q1673f: CPU 회귀 원인 가르기)과 2번(Core EVENTS 수정)이다. 0.30.1 job이 돌고 있으면 측정 job은 zlink-cf와 시점을 맞춘다.
- 진행 상태가 바뀔 때마다 Project 보드와 이슈를 갱신하고, 언어별 바인딩 대비 비율 표와 목표 달성 가능성을 내게 보고한다.

