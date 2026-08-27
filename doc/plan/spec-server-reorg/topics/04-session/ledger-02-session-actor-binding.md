# 규칙 등가성 대장 — session-actor-binding.ko.md

> [session-pilot-mapping.ko.md](mapping.ko.md) §5 R18–R60의 새 위치를 기록한다.
> 대상 문서: `framework/doc/framework/common/spec/server/session/session-actor-binding.ko.md`.

| R# | 새 위치 (절 번호·제목) | 비고 |
|---|---|---|
| R18 | §3 Startup 조건 | `EnableActorDispatch`·role·Store·hidden binding 없음 |
| R19 | §1 개요(질문·계약 표) + §4 Binding이 잇는 값 | N:1 관계. 요약(§1)과 상세(§4) 두 층에 등장 — guide §4.3에 따른 층 분리이며 중복 서술이 아니다 |
| R20 | §1 개요(질문·계약 표) | Relay는 bind route 사용, message마다 Store 조회 없음 |
| R21 | §5 Bind와 relay | envelope 보존 값 6개(원본 request correlation, binding token, Actor `ObjectGeneration`, `AuthorityOwnerGeneration`, `OwnerLeaseGeneration`, session sequence) — bullet 목록으로 §5 도입부에 위치 |
| R22 | §5 Bind와 relay | Actor queue 직접 제출, `Current Spot은 authority 검증에 사용하지만 callback 실행 문맥이 아니다`, session thread 미실행, global 직렬화 없음 |
| R23 | §5(terminal-once 완료) + §12(자동 재전송 금지) | 완료 서술과 실패 서술을 층으로 분리했다 |
| R24 | §12 실패와 오류 | late reply를 새 session/binding에 사용 안 함 |
| R25 | §4 Binding이 잇는 값과 보관하는 정보 | destroy → binding 종료, 새 incarnation에 이전 token 무효 |
| R26 | §4 Binding이 잇는 값과 보관하는 정보 | 보관 정보 5행 표 |
| R27 | §5 Bind와 relay | bind = control request 1회(38), terminal reply 1회 |
| R28 | §5 Bind와 relay | 24/36 record의 generation 검증 |
| R29 | §5 Bind와 relay | 사전 Store 조회·local overload 없음 |
| R30 | §5 Bind와 relay | 저장 route 무효 시 Message Follow 1회 또는 `Unavailable` |
| R31 | §5 Bind와 relay | route 유효 범위 = owner lease + admission deadline |
| R32 | §4 Binding이 잇는 값과 보관하는 정보 | binding identity 3요소, 같은 lifecycle 안에서만 비교 |
| R33 | §6 Rebind와 이전 연결 교체 | rebind 순서, terminal은 이전 session 대기 없음 |
| R34 | §6 Rebind와 이전 연결 교체(다이어그램 포함) | 51 one-way, 100 ms, closing 전이, timer 재확인, deadline 강제 종료 |
| R35 | §6 Rebind와 이전 연결 교체 | 51 전송 실패 diagnostics, bounded retry, rollback 없음 |
| R36 | §6 Rebind와 이전 연결 교체 | tombstone, malformed control 처리 |
| R37 | §6 Rebind와 이전 연결 교체 | idempotent 재제출, close 시 다른 binding 정리 |
| R38 | §4 Binding이 잇는 값과 보관하는 정보 | 같은 ObjectGeneration 유지 relocation은 rebind 아님; destroy 후 새 generation은 explicit bind |
| R39 | §6 Rebind와 이전 연결 교체 | 다른 owner/generation rebind atomic 등록 + one-way 51 |
| R40 | §5 Bind와 relay | target에 Actor 없음 4분기 + `BindOrGet`의 Get 범위 |
| R41 | §5 Bind와 relay | bound-session API(push·close만, global proxy 없음, needle 1) |
| R42 | §7 Disconnect 통지 | snapshot 고정 → all-settled |
| R43 | §7 Disconnect 통지 | Entry/User Spot 통지, needle 2+3 |
| R44 | §8 Actor relocation 중 Session의 책임(도입부) | socket·session state 이동·복제 없음, target 선택·Store 접근 없음 |
| R45 | §8.1 Seal, held message와 route 전환 | Session owner 단독 검증 값 4개 |
| R46 | §8.1 Seal, held message와 route 전환 + §8.2 Control message 42·43·44 | seal 42→43, 다른 binding 무영향 |
| R47 | §8.1 Seal, held message와 route 전환(one-way·TCP 순서 의존 문단) + §8.2 Control message 42·43·44 | 44 commit: route+snapshot 원자 갱신 → held 제출 → seal 해제, 45 미사용(reserved, 이름 미기재), duplicate no-op; cutover·44는 one-way라 response 유실 없음 — TCP 순서·재전송 의존, `send`는 ACK 없음, `request`는 기존 correlation·deadline·caller retry 계약 유지 |
| R48 | §8.1 Seal, held message와 route 전환 | `SessionRelocationSealTimeout` 기본 3,000 ms, timeout·44 같은 직렬 구간, late Warning |
| R49 | §8.1 Seal, held message와 route 전환 | relay-ready 전 abort → source coordinator가 44 abort one-way 전송, Session owner는 matching seal 해제 후 source route로 제출(20/48 순서 유지), reply 없음, source route 재개방 없음(이후) |
| R50 | §9 재접속과 이동의 구분(말미) | CAS 뒤 rollback 없음, 전역 순서 미보장, disconnect는 증거 아님 |
| R51 | §10 실행과 수명 | session owner 직렬화, Actor 제출 뒤 Actor queue가 순서 소유 |
| R52 | §10 실행과 수명 | completion 등은 infrastructure task |
| R53 | §10 실행과 수명 | Session owner host Relocate/Shutdown |
| R54 | §3 Startup 조건(startup 행) + §12 실패와 오류(operation 행) | 표를 두 절로 분리했다. Startup 3행은 §3, operation 6행은 §12 |
| R55 | §5 Bind와 relay(도입부 결정 2개) | session gate ≠ Actor gate; 제어 record는 application queue 밖 |
| R56 | §11 실행 engine과 lane 정책 타입 | 실행 engine 하나, lane 정책 타입, 언어별 재량(sealed/tagged union) |
| R57 | §9 재접속과 이동의 구분 | 재접속=새 session, 이동=연결 유지 |
| R58 | §8 Actor relocation 중 Session의 책임(도입부 결정) | relocation seal ≠ retired binding 거부 |
| R59 | §8.1 Seal, held message와 route 전환 | numeric high-water 없음, 개별 message 제한은 유지 |
| R60 | §14 검증 요구 | 아래 세부 대응표 참고. Reviewer 지시로 "Target CAS 시점" 행 삭제(target-side 규칙은 relocation 주제 소유 — 28 §4 / 52 §7이 이미 서술) — 이 문서는 target CAS 자체를 검증 항목으로 갖지 않는다. "Late/duplicate 무해성" 행은 command 44로 범위를 좁혔다(cutover late/duplicate 관찰은 relocation 주제 소유, 28 §4 / 52 §7). |

## R60 세부 대응 — 옛 검증 항목 → §14 새 행

옛 20 §9는 15항목이다(매핑 문서 §5 R60 주석의 "16항"은 재작성 중 다시 세어 보니 15항목이었다 —
아래 spec-gap 후보 참고).

| 출처 | 옛 항목(요약) | §14 새 행 |
|---|---|---|
| 20 §9 #1 | MeshName 없이 global Actor authority | Enablement 범위 |
| 20 §9 #2 | Object role/Store 없으면 startup 거부 | Startup 거부 |
| 20 §9 #3 | Session 하나에 여러 Actor, 독립 route/token | 다중 binding |
| 20 §9 #4 | Local·remote payload 직접 전달, Spot callback 안 거침 | 직접 제출 |
| 20 §9 #5 | Bind 1회 제출, hidden retry 없음 | Bind 재시도 없음 |
| 20 §9 #6 | Bind 뒤 저장 route 사용, message마다 조회 없음 | Route 재사용 |
| 20 §9 #7 | Disconnect all-settled, `OnDisconnectActorAsync` 최대 1회 | Disconnect all-settled |
| 20 §9 #8 | Rebind 뒤 이전 token·fence가 current를 안 바꿈 | Rebind 격리 |
| 20 §9 #9 | command 38/24/36 raw ROUTER 경로 | Wire 경로 |
| 20 §9 #10 | Reply가 original correlation으로 1회 완료 | Terminal-once reply |
| 20 §9 #11 | Physical connection·session object 비이동 | Connection 비이동 |
| 20 §9 #12 | Relocation commit 뒤 target 처리 시작, route 비동기 갱신 | Relocation route 갱신 |
| 20 §9 #13 | Bound-session request가 저장 작업 또는 hold relay에 포함 | Held request 처리 |
| 20 §9 #14 | Command 44 무응답, Message Follow duration 뒤 제거 | Command 44 무응답 |
| 20 §9 #15 | Relay-ready 전 failure만 source 복원 | Abort 경계 |
| 48 §5 #1 | 같은 연결 두 callback 동시 실행 안 함 | Session·Actor gate 분리 |
| 48 §5 #2 | Session callback 문맥에서 Actor handler 미실행 | Session·Actor gate 분리(같은 행에 병합) |
| 48 §5 #3 | 연결 유지 신호가 업무 message 뒤에 안 밀림 | 제어 record 우선순위 |
| 48 §5 #4 | 직렬 실행 원시 타입 하나 | 실행 engine 단일성 |
| 48 §5 #5 | 교체는 새 binding 등록으로 완료, ACK 대기 없음 | 교체 완료 조건 |
| 48 §5 #6 | 이전 callback terminal 100 ms 뒤 close | 100 ms 지연 close |
| 48 §5 #7 | 완료 응답 전까지 기존 경로 사용 | 교체 중 기존 경로 유지 |
| 48 §5 #8 | 늦은 응답이 교체 순번으로 필터링 | 교체 순번 필터 |
| 48 §5 #9 | 재접속 시 이전 연결 관계 비복원 | 재접속 비복원 |
| 48 §5 #10 | 이동 시 연결 유지, 경로만 갱신 | 이동 시 연결 유지 |
| 48 §5 #11 | 42/43이 seal 결과만 전달 | Seal reply 최소성 |
| 48 §5 #12 | `SessionBindingAggregate` 단일 검증, 재검증 없음 | 단일 검증 지점 |
| 48 §5 #13 | Target만 CAS 수행(cutover 또는 1,000 ms) | (삭제 — relocation 주제 소유, 28 §4 / 52 §7이 이미 서술) |
| 48 §5 #14 | CAS 성공 뒤 44 → route 적용 → held 제출 → seal 해제 | Route 적용 순서 |
| 48 §5 #15 | Relay-ready 전 abort는 reply 없이 44 one-way | Abort 응답 없음 |
| 48 §5 #16 | `SessionRelocationSealTimeout` 기본 3,000 ms | Seal timeout 기본값 |
| 48 §5 #17 | late/duplicate cutover·44는 Warning만 | Late/duplicate 무해성(command 44만 — cutover late/duplicate 관찰은 relocation 주제 소유) |
| 새로 추가 | Lane 정책 타입이 무의미한 조합을 만들 수 없음(48 §2 판정 기준의 관찰 가능한 대응) | Lane 정책 조합 |

새로 추가한 "Lane 정책 조합" 행은 48 §2의 판정 기준("의미 없는 조합을 만들 수 없는가")을
검증 요구 형태로 옮긴 것이며 §14의 언어별 재량 서술과 짝을 이룬다. 두 옛 문서의 항목 수는
15 + 17 = 32이며, 이 중 48 §5 #1·#2를 한 행으로 병합해 §14는 총 31행이다(추가 1행 포함 시
32행 관찰 대응).

## spec-gap 후보

수정하지 않고 사실만 기록한다.

1. **Abort 순서 표현이 문서마다 다르다.** 20 §5 step 9·§6과 48 §4는 "matching seal만 해제하고
   보관한 message를 source route로 제출"(seal 해제 → 제출) 순으로 적는 반면, 52 §5는
   "held message를 source route로 제출하고 그 seal만 해제한다"(제출 → seal 해제) 순으로 적는다.
   이번 문서(§8.1·§8.2)는 20/48의 순서를 그대로 따랐다 — 두 소스 문서가 일치했기 때문이다.
   52와의 표현 차이는 relocation 주제 차례에서 판정해야 한다. (출처: 20-session-actor-dispatch.ko.md
   §5 step 9·§6, 48-internal-session-binding.ko.md §4, 52-internal-relocation-handoff.ko.md §5)
2. **"lifecycle deadline"의 소유 문서가 없다.** 20 §4는 교체 callback이 lifecycle deadline 안에
   끝나지 않으면 강제 종료한다고 하고, §4.1은 automatic 통지가 lifecycle deadline 안에서
   callback terminal을 기다린다고 한다. 두 곳 모두 이 deadline 값이나 그 소유 스펙을 이름으로
   지정하지 않는다. (출처: 20-session-actor-dispatch.ko.md §4, §4.1)
3. **Explicit create error·typed stale error가 32 §2의 닫힌 `ErrorKind` 집합 중 어느
   값인지 명시하지 않는다.** 20 §8은 "Actor factory가 없다 → Explicit create error", "fence가
   stale하다 → Typed stale error"라고만 적는다. (출처: 20-session-actor-dispatch.ko.md §8;
   32-framework-error-model.ko.md §2)
4. **"bounded asynchronous retry"의 횟수·기간이 정해져 있지 않다.** 20 §4는 51 전송 admission
   실패 시 "exact retired identity별 bounded asynchronous retry"라고만 적고 상한을 밝히지
   않는다. (출처: 20-session-actor-dispatch.ko.md §4)
5. **Abort를 보내는 주체 이름이 문서마다 다르다.** 20 §5 step 9와 §6, 48 §4, 52 §5는 모두
   "source coordinator"라고 쓰는데, 20 §6 첫 문장만 "Relocation coordinator"라고 쓴다. 같은
   주체를 가리키는지 다른 role인지 소스에 명시되어 있지 않다. (출처: 20-session-actor-dispatch.ko.md
   §5 step 9, §6)
