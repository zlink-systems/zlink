# 04-interaction-model 대장 — R# → 새 위치

> 대상: `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md`
> (498줄). 옛 문서: `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md`
> (408줄). 매핑표: [mapping.ko.md §3.3, §5](mapping.ko.md#33-interaction-model-절-구성).
>
> "새 위치"는 새 문서에서 grep으로 핵심 문구를 실물 확인한 뒤에만 채웠다(가이드 §2.5).

| R# | 새 위치 | 비고 |
|---|---|---|
| R40 | §1 공통 모델 표 (9행 그대로) | |
| R41 | §1 표 아래 select-one 정의 문장 | |
| R42 | §2 상호작용 시작 interface 표(8행) | |
| R43 | §2 표 아래, `Send.../Request.../Yield<TReply>()` 문단 | |
| R44 | §3 첫 규칙 불릿("Node direct는 infrastructure와…") | |
| R45 | §3 둘째 규칙 불릿("Global Spot·Actor message는 cached Ready route…") | |
| R46 | §3 셋째 규칙 불릿("Channel operation은 ChannelName으로…") | |
| R47 | §3 넷째 규칙 불릿("Weight 0은 새 channel 선택에서…") | |
| R48 | §3 다섯째 규칙 불릿("Select-one은 첫 binding operation을…") | |
| R49 | §3 여섯째 규칙 불릿("Node direct는 RID, Spot·Actor는 global ID…") | |
| R50 | §3 일곱째 규칙 불릿("같은 ChannelName을 여러 물리 송신 경로에…") | |
| R51 | §4 첫 규칙 불릿("`send`는 비동기 submit 하나만…") | |
| R52 | §4 둘째 규칙 불릿("Queue가 일시적으로 가득 차면…") | |
| R53 | §4 셋째 규칙 불릿("Global Spot·Actor send도…") | |
| R54 | §4 넷째 규칙 불릿("Message call은 Missing object의…") | |
| R55 | §4 다섯째 규칙 불릿("유효한 one-way call은…") | |
| R56 | §4 여섯째 규칙 불릿("`request`는 선택한 송신 경로에…") | |
| R57 | §4 일곱째 규칙 불릿("Spot에서 시작한 request는…") | |
| R58 | §4 여덟째 규칙 불릿("같은 origin이 같은 destination pipe에…") | |
| R59 | §5 첫 문단("Logical Multicast publish는 target ChannelName…") | |
| R60 | §5 4행 목록(remote MeshNode마다 1회 submit … relay·replay 없음) | |
| R61 | §5 첫 규칙 불릿("Framework service runtime은 bounded I/O executor에…") | |
| R62 | §5 둘째 규칙 불릿("Transaction 시작이 snapshot operation의 commit point…") | |
| R63 | §5 셋째 규칙 불릿("Snapshot target이 모두 0이어도…") | |
| R64 | §5 넷째 규칙 불릿("Publish 정상 완료는 transaction을 시작했다는 뜻…") | |
| R65 | §6 첫 문단("classic fanout은 MeshNode와 독립된…") | |
| R66 | §6 첫 규칙 불릿("Publisher call은 publisher socket send timeout까지…") | |
| R67 | §6 둘째 규칙 불릿("Publish의 공통 입력은 ChannelName, topic과…") | |
| R68 | §6 셋째 규칙 불릿("Publisher는 전용 location descriptor에…") | |
| R69 | §7 첫 규칙 불릿("Entry Spot과 Instance Spot의 direct message…") | |
| R70 | §7 둘째 규칙 불릿("Instance Spot은 Actor membership이 없는…") | |
| R71 | §7 셋째 규칙 불릿("`ActorRef`와 `SpotRef`는 global ID…") | |
| R72 | §7 셋째 규칙 불릿 후반("Bound session의 `Ref`/`ref()` accessor는…") | |
| R73 | §7 넷째 규칙 불릿("Actor message는 global Actor ID의 current authority를…") | |
| R74 | §7 다섯째 규칙 불릿("Node·Spot·Actor와 binding operation completion은…") | |
| R75 | §8 첫 규칙 불릿("Framework 내부 recv loop는 packet을…") | |
| R76 | §8 둘째 규칙 불릿("Session과 Actor가 bind되면…") | |
| R77 | §8 셋째 규칙 불릿("Server package의 bound session send…") | |
| R78 | §8 넷째 규칙 불릿("Reply sequence 또는 one-shot token이…") | |
| R79 | §10 두 규칙 불릿(reply route 복원·drop, handler 예외 기록) | |
| R80 | §11 네 규칙 불릿(Relocating/Shutdown 제한 → Draining deadline → 이동 불가/이동 허용 → admission seal 검증) | |

## 이동 후 갱신할 링크

문서를 최종 위치로 옮기는 시점(§5)에 갱신해야 하는, 아직 이동하지 않은 문서로의 링크다.
이 문서는 이번 재작성에서 아직 존재하지 않는 두 같은-주제 파일(`02-glossary.ko.md`의 다수
anchor, `README.ko.md`)을 상대 경로로 가리킨다 — 같은 `00-foundation/` 병렬 작업이 만드는
파일이므로 이동 대상은 아니지만, 그 파일들이 아직 없어 `check_doc_links.py`가 "링크 대상
없음"으로 보고한다(§"Finish" 참고). 이 문서는 `06-framework-api.ko.md`를 직접 링크하지
않는다.

옛 문서가 참조하던 파일 중 이 주제 밖에 있어 아직 옮기지 않은 문서는 없다 — 참조한
`languages/dotnet/interfaces/*`, `../04-session/01-stream-session.ko.md`,
`../04-session/02-session-actor-binding.ko.md`, `07-framework-error-model.ko.md`는 모두 이미
현재 경로에 존재한다(session 주제는 이미 이동 완료).

| 링크 | 새 문서에서의 표기 | 상태 |
|---|---|---|
| `02-glossary.ko.md#…` (다수) | 그대로 유지 | 대기 — 같은 주제의 `02-glossary.ko.md`가 아직 작성되지 않음(병렬 작업) |
| `README.ko.md` (Foundation 주제 목차) | 그대로 유지 | 대기 — `00-foundation/README.ko.md`가 아직 작성되지 않음 |
| `../README.ko.md` (스펙 목차) | 그대로 유지 | 대기 — 전체 목차 `spec/server/README.ko.md`가 아직 작성되지 않음(§5 마지막 단계) |
## spec-gap 후보

옛 문서의 규칙(R40~R80)은 새 문서에 1:1로 대응하며, 추가하거나 완화한 보장은 없다. 대조
중 발견해 판정이 필요한 후보는 다음 두 건이다.

- **`interaction-model` §1과 `message-model` §2의 Send·Request 완료 서술이 여전히
  중복된다.** 이 캠페인의 지침(에이전트 프롬프트)은 "이 문서가 상호작용-수준 완료 표를
  소유한다"고 지정했지만, 이미 작성된 `05-message-model.ko.md` §2도 Send·Request·Logical
  Multicast·Classic fanout·STREAM의 완료 조건을 담은 표를 갖고 있다(예: Send 행 "Source-local
  queue가 수락하면 반환 데이터 없이 완료하며…"가 이 문서 §1의 node direct send 행과
  거의 같은 문장). mapping.ko.md §4 S5가 원래 없애려던 중복이 두 문서 모두 완성된 뒤에도
  남아 있다 — 소유 판정이 필요하다.
- 옛 문서 §5 "Framework service runtime은 bounded I/O executor에…"가 참조하는 bounded I/O
  executor의 slot 수·정책은 이 문서(그리고 옛 03-interaction-model)가 애초에 정의하지 않는다
  — 값 자체가 없는 것이지 이번 재작성이 만든 gap은 아니다. 참고용으로만 남긴다.
