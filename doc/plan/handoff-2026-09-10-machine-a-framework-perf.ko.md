# Handoff — 머신 A 새 세션: gRPC 벤치와 framework 성능 0.90 (milestone 0.18.0)

> 작성: 2026-09-10 저녁. 이전 세션 기록은 [`handoff-2026-09-10-framework-perf.ko.md`](handoff-2026-09-10-framework-perf.ko.md)(§0이 저녁 상태)와
> [`fw-bench-worklog/decisions.ko.md`](fw-bench-worklog/decisions.ko.md) FB-059~FB-065. 바인딩 submit 결과 객체 캠페인은 **머신 B**가 한다
> ([`handoff-2026-09-10-machine-b-submit-result.ko.md`](handoff-2026-09-10-machine-b-submit-result.ko.md)). 이 세션은 그 캠페인의 코드를 건드리지 않는다.
> 규범: `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.{ko,en}.md`, 06(state lane), 벤치 규격 `framework/bench/grpc/README.{ko,en}.md`.

## 1. 목표와 지금 숫자

- **목표(사용자)**: framework send·request 처리량 ≥ 같은 언어 raw binding의 **0.90**. 판정 패턴은 규격 §7.2(request-backpressure 기준, §5.2 깊이·지연 병기).
- Java request-serial 1024(1-run, 2026-09-10): raw 8,784/s(perf 구조) · gRPC 5,929 · framework **3,238**(#85 뒤) → 0.37. 제출 구간 38 µs, 완료 구간 framework 55 vs raw 40 µs.
  남은 격차는 제출 이후 서버 쪽 19포인트와 클라이언트의 스레드 전달(요청당 12회 vs raw 3회)이다(`.artifacts/codex/java-completion-profile/full-report.md`).
- 다른 언어 위치는 이전 handoff §5. C++ serial 0.13(−20% 회귀 알고 머지), .NET send 3배·window 2배 개선 뒤 I0 대기 미완, Node는 framework 벤치 클라이언트 없음(#10).

## 2. 이 세션이 맡는 Issue (전부 milestone 0.18.0)

| 묶음 | Issue | 상태·다음 행동 |
|---|---|---|
| Java framework perf | **#85** ToNode lane 1회 | job 완료 `dcd82163eb`(worktree `zlink-85-*`, 171→38 µs, +40%). 재검증이 공유 Maven에 binding 0.18.0이 없어 막힘 → 로컬 패키지(`scripts/local-package/java/build-wsl.sh`) 만든 뒤 `:zlink-framework-core:test` 재실행 → PR(`Closes #85`) → 머지. **B의 #97보다 먼저.** |
| | #47 복사 25회·실행기·컨테이너 탐색·timer, #6 서비스 펌프 1 ms 폴링, #69 send-saturation ROUTE_NOT_CONNECTED, #45 envelope header(4언어) | job 병렬 가능(파일군이 다름). 각각 Issue → worktree → job → PR. |
| .NET | #48 lane 왕복 14회·복사 4회·record별 task·DI scope, #5 진단·0.90, #19 admission 거절 표면화, #17 macOS drain hang, #60 oversized reply | #48부터. #19는 spec 01 §5(`Backpressured`는 public result 아님)를 지키는 방향. |
| C++ | #49 lane 7회·host mailbox·복사 8회, #7 진단·0.90, #8 send target 소실, #9 HTTP bind 실패 abort, #23 samples(Windows 작업자) | #49의 serial −20% 회복 먼저. |
| Node | #50 ingress 1 ms 타이머·1건 batch, #10 codec bytes(framework 벤치 행 선행 조건), #18 Windows E2E, #77 macOS bootstrap, #82 Rejected→internal_failure | #10 → 벤치 행 활성화 → #50. |
| bench | #13 나머지 언어(.NET·C++·Node의 raw ROUTER↔ROUTER RID 직접·framework ToNode·request-window 제거), #46 JAVA_HOME, #37 비교표 도구, #16 PR CI 워크플로우(.github는 감독자) | #46은 5분짜리. #13 언어 확대는 B의 raw 드라이버 변경(#91~#93)과 겹치므로 **gRPC·framework 행만** 먼저 하고 raw 행은 B 뒤. |
| bindings(A 소유) | #11 Node window 유실, #14 Java Unsafe→FFM 회귀 9건, #15 MaxMessageSize 정리, #75 Java 가상 스레드 multipart | B의 binding 변경과 같은 파일이면 B 뒤로 미룬다. #14·#75는 Java binding 내부라 **B의 #89 머지 뒤**. |

착수 순서 권고: #85 PR → (병렬) #47·#48·#49·#50/#10·#46 → #45 → #13 gRPC·framework 행 → 나머지 결함. 동시 codex job은 5개까지, perf 측정은 큐가 직렬화한다.

## 3. B와의 경계 (반드시 지킨다)

- A는 framework의 **binding 호출 줄**(`router.request(...).submit()`류)과 messaging call **종결자 시그니처**를 바꾸지 않는다. B가 `.reply()`/`.admitted()` 치환과 동기 종결자를 넣는다.
- A는 `bindings/**`, `bindings/doc/**`, `doc/perf/PERF_*`, framework 01장 §2·§4·§5·§15·§16, 언어별 interface 문서를 바꾸지 않는다.
- A가 바꾸는 곳: framework runtime 내부(lane·pump·registry·envelope·dispatch·worker), 벤치 gRPC·framework 드라이버·러너·비교 도구, `framework/bench/grpc/README.*`, 08장.
- 같은 언어의 framework 파일에 B의 열린 PR이 있으면 머지 순서를 맞춘다(`gh pr list`). B가 binding 4언어 태그를 내면 A의 재검증·framework 릴리스가 그 패키지를 쓴다.

## 4. 반복하지 말 것 (이번 세션에서 확정)

- **raw 클라이언트가 무너지면 이론을 세우기 전에 `bindings/<lang>/perf`의 같은 패턴과 비교한다**(FB-065). 벤치에 application 상한을 넣지 않는다. 측정 조건(warmup·HWM·timeout)을 완화하지 않는다.
- HWM은 byte 기준(manual 기본 4,096,000, auto는 budget water-filling). 메시지 건수로 환산하지 않는다.
- POLLOUT은 "읽지 않은 WRITABLE record 있음"(재시도 힌트)이지 "보낼 수 있음"이 아니다.
- gRPC request-backpressure 20만/s는 미완료 100만 건·지연 5~21 s에서 나온 값이다. 표에는 §5.2대로 깊이·지연을 같이 싣는다.
- "기능이 없다"는 job 보고는 perf에서 같은 패턴을 인용시킨 뒤에만 받는다. codex/astra 판정은 스펙·코드로 재검토한 뒤 채택한다.
- 스펙 06 §3: lane 밖에서 읽고 비동기로 행동하지 않는다. "한 turn 안에서 조회+선택+제출"이 정답이었다(#78·#85).

## 5. 결과를 받는 법·자주 쓰는 명령

이전 handoff §2 "결과를 어디서 받나"와 §8 그대로. 요약:

```bash
bash scripts/dev/session-setup.sh --check
bash scripts/dev/job.sh status | start <이름> --worktree <경로> --brief <파일> --model gpt-6-astra --effort high
bash scripts/dev/work.sh start --issue N --no-packages ; ln -s ~/project/zlink/.artifacts/wsl <worktree>/.artifacts/wsl
bash scripts/dev/work.sh pr --body <파일> --refs|--closes      # worktree 안에서, 본문에 '## 검증' 절 필수
bash scripts/dev/work.sh done --verified <sha>                 # worktree 안에서; 끝나면 cd ~/project/zlink
bash scripts/perf/perf-ticket.sh submit -p 1 -o supervisor -d '<설명>' -- env JAVA_HOME=/home/hep7/.cache/zlink/jdk/temurin-25 <명령>
```

- job 결과는 `.artifacts/codex/<job>/summary.md`·`pr-body.md`. 3분 감시자가 완료·실패만 알린다. PR 전 감독자가 테스트를 직접 한 번 돌리고 diff를 읽는다.
- framework-java CI는 PR 트리거가 없다(#16). 로컬 검증으로 머지한다. framework-dotnet·node CI는 Core 0.18.0 릴리스 자산이 올라온 뒤부터 초록이 된다.

## 6. 릴리스 관계

- Core 0.18.0: 태그·빌드는 냈다. 자산 확인 → `~/.cache/zlink/core/0.18.0` prefix → Conan·vcpkg SHA 갱신(#87 잔여, `release-check.sh core 0.18.0`).
- bindings 0.18.0 태그는 B. framework 릴리스(0.12.0 후보, MINOR: 동기 종결자 추가)는 A·B가 모두 main에 들어간 뒤 `framework-<lang>/vA.B.C`.

## 7. worktree

유지: `zlink-85-*`(PR 대기), `zlink-12-*`(B의 #90 base — 건드리지 않음), `zlink-87-*`·`zlink-102-*`(머지됨, `work.sh done` 뒤 제거).
정리 가능: `zlink-13-bench-pairing`, `zlink-fwclient-completion`, `zlink-fwclient-profile`, 그 밖의 이전 handoff §7 목록.
