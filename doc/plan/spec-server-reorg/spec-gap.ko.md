# spec/server 재구성 — spec-gap 대장

> 재구성 중 발견한 **실제 스펙 결함**을 기록한다. 재작성으로 생긴 문서 오류는 여기 적지 않고
> 그 자리에서 고친다. 여기 적힌 항목은 재구성 범위 밖이며
> [공개 계약 절차](../../../framework/doc/framework/common/spec/server/00-foundation/01-public-contract-governance.ko.md#5-공개-계약-절차)로
> 따로 처리한다. 등록 시점의 문서는 **계약 의도**대로 두고 구현을 바꾸지 않는다.

상태: `후보` (읽으면서 발견, 구현 미확인) → `확인` (4언어 대조 완료) → `판정` (오류/개선/기각) → `이관` (governance 이슈 번호)

종류 — 처리 경로가 다르므로 구분한다.

이 대장의 모든 행은 spec gap — 스펙과 구현이 어긋난 상태 — 이다. 종류는 **무엇 때문에
어긋났는가**로 나누고, 해소 수단(스펙을 고치는가, 구현을 고치는가)은 그에 따라 정해진다.

| 종류 | 어긋난 이유 | 해소 수단 | 이번 재구성 캠페인에서 |
|---|---|---|---|
| 미지정 | 스펙이 값·규칙을 정하지 않아 언어마다 다르게 구현함 | 스펙에 값 명시 + 그 값과 어긋난 구현 수정 | 명시까지 |
| 모순 | 스펙 문서끼리 또는 한 문서 안에서 규칙이 충돌함 | 소유 문서 기준으로 스펙 정정 | 정정까지 |
| 불일치 | 스펙은 명확한데 구현이 다르게 동작함 | 구현 수정(스펙 변경 정책상 스펙 완화는 불가) | 대장 등록만 — 구현 캠페인으로 |

구현을 이번 캠페인에서 고치지 않는 이유: gap은 재구성이 만든 것이 아니라 옛 스펙 시점부터
있던 것이고(재작성 오류 0건), 구조 변경과 동작 변경을 한 흐름에 섞으면 회귀 원인을 분리할 수
없기 때문이다. 예외는 문서를 믿고 쓰면 장애가 되는 항목 — 별건으로 먼저 처리할지 판단한다.

처리 순서.

1. **구현 상태 기록** — 주제 루프의 4언어 gap 검토 때 G행도 함께 준다. 미지정 항목은 일치/불일치가
   아니라 "그 언어가 실제로 무엇을 하는가"(값·동작·파일:줄)를 언어별 열에 적는다. 이 시점에
   스펙은 원문 그대로 두고 상태를 `확인`으로 올린다.
2. **판정** — 4언어가 같으면 스펙 개선(값 명시, 코드 변경 없음). 언어마다 다르거나 아무 언어도
   구현하지 않았으면 계약 의도를 정해야 하므로 근거와 함께 사용자에게 올려 결정받는다.
판정은 주제마다 고민하지 않는다. 주제 루프는 `후보`·`확인`까지만 하고 다음 주제로 넘어간다.
주제가 끝날 때 한 번 판정표를 만든다 — 4언어가 같은 항목은 "스펙에 값 명시"로 자동 판정하고,
언어마다 다르거나 미구현인 항목만 추천안과 함께 사용자에게 올린다. 답이 없어도 다음 주제
진행을 막지 않는다.

3. **스펙 지정** — 판정된 항목만 스펙에 적는다. 재구성 커밋과 섞지 않고 주제별 spec-gap 해소
   커밋으로 분리한다. 어긋난 구현은 별도 구현 작업으로 넘긴다.

각 행은 판정 시점에 **추천**(내가 제안하는 처리)과 **결정**(사용자가 답한 결과)을 함께 적는다.
결정 열이 `대기`인 행만 사용자 답이 필요하고, `자동`은 4언어가 일치하거나 명백한 스펙 오류라
답 없이 처리한다. 근거와 언어별 상세는 [session 판정표](topics/04-session/judgment.ko.md).

## 사용자 결정이 필요한 항목 — D1~D10

아래 표의 `추천`·`결정` 열에 나오는 `D#`는 이 목록을 가리킨다. 각 항목의 언어별 근거와
판정 과정은 [session 판정표](topics/04-session/judgment.ko.md) 34~44행에 있다.

| D# | 관련 G행 | 무엇을 정해야 하는가 | 추천 | 상태 |
|---|---|---|---|---|
| D1 | G9 | "서로 다른 Actor를 Spot global queue로 직렬화하지 않는다"가 Actor 모델의 `SpotWide` 공통 gate와 모순 | 스펙 오류 정정 — "session의 실행 문맥으로 직렬화하지 않는다. Actor 사이의 순서는 Actor 모델의 실행 모드가 정한다" | **적용 완료** — 명백한 모순이라 대장 기준상 자동 처리. `04-session/02-session-actor-binding.ko.md` 2곳(§1 도입부·§4 payload 전달) |
| D2 | G10 | commit 순서를 "route 적용 → held 제출 → seal 해제"로 둘 것인가 | 순서 대신 관찰 보장으로 개선 — "held는 seal 해제 뒤 도착한 message보다 먼저 제출된다". 코드 변경 없음 | **적용 완료** — 4언어 전부 일치라 대장 기준상 자동 처리. `04-session/02-session-actor-binding.ko.md` §8.2. 구간 안 순서는 `**언어별 재량**`으로 표시 |
| D3 | G11 | late·duplicate command 44에 Warning을 남기는 규칙을 유지할 것인가 | 스펙 유지 + 3언어에 Warning 추가(jvm이 선례). 운영자가 relocation 지연을 볼 유일한 신호 | **문서 변경 없음 — 구현 부채로 등록** |
| D4 | G6 | "같은 session type 중복 등록"이 서로 다른 node 사이를 뜻하는가 | cpp의 두 검사를 계약으로 채택하되 범위를 **host 단위**로 명시 — (a) 한 node에 둘 이상, (b) 같은 host의 두 node에 같은 type | **적용 완료** — dotnet·jvm·node에 host 단위 검사 추가가 부채 |
| D5 | G1 | 교체 callback 강제 종료 deadline의 기본값 | server option `SessionReplacementCallbackTimeout`으로 명시. 기본값 **30,000 ms**(사용자 결정) | **적용 완료** — jvm·cpp 하드코딩 5초 제거가 부채 |
| D6 | G2(a) | "Actor factory가 없다"의 `ErrorKind` | `NotFound` — 오류 모델 32 §2 정의에 가장 가깝고 cpp 기존 동작 | **적용 완료** — dotnet·jvm·node 오류 코드 변경이 부채 |
| D7 | G3 | 51 전송 실패 시 retry 상한 | **값 선택이 아니라 중복 규칙 제거** — admission은 이미 `04-interaction-model` §4·`06-framework-api`가 "send timeout까지 대기 후 `DeadlineExceeded`"로 정한다. 그 위의 별도 재시도 요구를 걷어냄 | **적용 완료** — dotnet·node(30초 재시도)·cpp(4회 재시도) 제거가 부채. jvm이 계약에 맞음 |
| D8 | G12 | admission 실패 시 receive 중단 범위 | **미지정도 불일치도 아니었음** — `01-execution/05` §6이 이미 host 공유 permit과 지원 socket 전체 적용을 정하고, cpp도 `application_job_queue.hpp`에서 `receive_flow_sockets` 전체에 fan-out 한다(직접 확인). 1차 대조의 "cpp만 연결별"이 오관찰이었다 | **적용 완료** — session 문서에 범위 문장·링크 보강. 구현 부채 없음. 회귀 테스트 추가(cpp) |
| D9 | G13 | bind 시 Message Follow relay 규칙 | 4언어 어디에도 구현 흔적이 없어 계약 의도 자체를 재확인해야 한다. Message Follow를 소유한 relocation 주제에서 사실 확정 후 판정 | **보류 — relocation 주제로 이월** |
| D10 | G16 | STREAM `MaxMessageSize` 초과 로그에 `EMSGSIZE` 리터럴을 요구하는가 | 스펙 유지 — 동작은 4언어 동일하고 로그 문자열만 dotnet이 다르다. 3언어가 이미 `EMSGSIZE`를 남기므로 운영 grep 이식성을 위해 이름을 유지 | **문서 변경 없음 — dotnet 구현 부채로 등록** |

## Gap 대장

| # | 주제 | 종류 | 스펙 위치 | 내용 | 언어별 상태 (dotnet / jvm / cpp / node) | 상태 | 추천 | 결정 |
|---|---|---|---|---|---|---|---|---|
| G1 | session | 미지정 | 20 §4 (R34) | 교체 callback이 "lifecycle deadline" 안에 terminal이 안 되면 강제 종료 — 이 deadline이 어느 값인지(용어집 `deadline`? Actor lifecycle deadline? 별도 설정?) 문서가 지정하지 않음 | dotnet 30 s(`DefaultRequestTimeout`) / jvm 5 s 하드코딩 / cpp 5 s 하드코딩 / node 30 s(override 가능) | 확인 → 결정 필요 D5 | server option `SessionReplacementCallbackTimeout`으로 명시. 기본값 30,000 ms(D5) | 적용 완료 (D5) |
| G2 | session | 미지정 | 20 §8 (R54) | "Actor factory가 없다 → Explicit create error", "fence stale → Typed stale error" — `ErrorKind`(32)로 어떤 값인지 미지정. 다른 행은 `Unavailable`·`InvalidOperation`을 명시함 | (a) factory 없음: dotnet `InternalFailure`·`Rejected` / jvm `NOT_CONFIGURED` / cpp `NotFound` / node config exception. (b) stale fence: 4언어 모두 `Unavailable`(jvm은 spot generation stale만 `INVALID_OPERATION`) | 확인 → (a) D6 적용 완료, (b) 자동 판정 J2 | (a) factory 없음 → `NotFound`(D6). (b) stale fence → `Unavailable`로 명시 | (a) 적용 완료 (D6) / (b) 적용 완료 — 재작성 트리(04-session/02-session-actor-binding.ko.md 등)에 이미 `Unavailable`로 일관되게 적혀 있어 잔여 미명명 표현 없음(수정 불필요) |
| G3 | session | 미지정 | 20 §4 (R35) | 51 전송 admission 실패 시 "bounded asynchronous retry" — 횟수·간격·상한이 없음. 언어마다 다를 가능성 | dotnet 10 ms 고정·30 s / jvm **retry 없음** / cpp 4회·10→40 ms·queue 1024 / node 25 ms→2배·cap 1 s·30 s | 확인 → 일반 admission 규칙과 중복이었음 | 별도 재시도 요구 삭제, send timeout·`DeadlineExceeded`로 일원화 | 적용 완료 (D7) |
| G4 | session | 모순(소유) | 19 §6 / 25 §4 | Session 종료 사유 집합의 소유가 Stream Connector §6.3(client 스펙)에 있음 — server 스펙이 client 스펙을 계약 출처로 가리킴. 소유 방향 확인 필요 | dotnet·cpp·node 1:1 / jvm wire 4종·계기 label 상이(→observability 이관) | 판정: 기각(소유 그대로) | 소유 그대로(client 스펙), 문서는 링크 유지 | 자동 — 기각 |
| G5 | session | 모순(소유) | 19 §10, 48 말미 | shared permit 규칙이 두 문서 끝에 번호 없이 덧붙음 — 33·46과 세 곳 서술. 소유 문서 확정 필요(execution 주제에서 처리 예정, 여기선 추적만) | 문서 소유 문제, 구현 무관 | execution 주제로 이월 | execution 주제가 소유 | 자동 — 이월 |
| G6 | session | 미지정 | 19 §7.2 (R12·R14) | startup 표의 "같은 session type을 중복 등록했다" 행과 "한 node에 session을 둘 이상 등록했다" 행의 관계가 불명확 — node당 session 하나(R12)면 전자는 후자에 포함됨. 전자가 서로 다른 node에 같은 type을 등록한 경우를 뜻하는지 원문 미지정 | dotnet 검사 없음 / jvm 검사 없음 / cpp **전역** 검사 / node 검사 없음 | 확인 → 결정 필요 D4 | 두 행을 node 단위·host 단위 검사로 분리해 명시(cpp 채택) | 적용 완료 (D4) |
| G7 | session | 모순 | 20 §6·48 §4 vs 52 §5 | abort 시 seal 해제와 held 제출 순서가 문서마다 다름 — 20 §5(해제→제출)·48 §4(해제→제출) vs 28 §5·52 §5(제출→해제). 4언어 구현은 모두 해제→제출 | dotnet held 폐기(J9) / jvm 해제→제출 / cpp 해제→제출 / node 해제→제출 | 확인 — **28도 52와 같은 순서**(1차 판정의 "52만 이탈"은 오류) | 4언어가 모두 해제→제출이므로 그 순서로 통일. session·relocation 재작성본은 이미 해제→제출로 씀 | 적용 완료 — 03-spot-actor/08-routing.ko.md의 역순 서술(제출 후 해제)을 해제→제출로 정정. session/02, 05-location-relocation/01·04는 이미 해제→제출로 서술돼 있어 그대로 둠 |
| G8 | session | 모순(용어) | 20 §5·§6, 48 §4, 52 §5 | abort를 보내는 주체가 "source coordinator"와 "Relocation coordinator"로 혼용됨(20 §6 한 곳). 같은 주체인지 원문 미지정 | 코드에 두 이름 모두 없음. wire 필드 `coordinator`·`senderRole` | 판정: 자동 J4 — 용어 통일 | "relocation coordinator(source node가 채우는 wire fence)"로 용어 통일 | 자동 |
| G9 | session | 모순 | 20 §3 (R22) | "서로 다른 Actor를 Spot global queue로 직렬화하지 않는다"가 Actor 모델 14의 `SpotWide` 공통 gate와 모순 | dotnet SpotWide 직렬화 / jvm ✔ / cpp eager coroutine 첫 turn inline / node ✔ | 확인 → 결정 필요 D1 | 스펙 오류 정정 — "session 실행 문맥으로 직렬화하지 않는다; Actor 간 순서는 Actor 모델 실행 모드가 정한다"(D1) | 적용 완료 (D1) |
| G10 | session | 불일치 | 20 §5 (R47) | commit 순서 "route 적용→held 제출→seal 해제" vs 3언어 모두 "해제 후 제출"(같은 직렬 구간, 관찰 결과 동일) | 4언어 모두 해제→제출 | 확인 → 결정 필요 D2 | 순서 대신 관찰 보장으로 개선 — "held는 seal 해제 뒤 도착한 message보다 먼저 제출"(D2) | 적용 완료 (D2) |
| G11 | session | 불일치 | 20 §5·§5.1 (R48) | late/duplicate command 44에 `late_session_route_update` Warning — 3언어 모두 미기록. dotnet은 다른 relocation의 late 44에 예외 throw | dotnet debug+throw / jvm Warning ✔ / cpp 없음 / node 없음 | 확인 → 결정 필요 D3, dotnet throw는 불일치 J12 | 스펙 유지 + 3언어에 Warning 추가(jvm이 선례)(D3) | 문서 변경 없음 — 구현 부채 (D3) |
| G12 | session | 미지정(문서) | 19 §4 (R2) | admission 실패 시 "새 packet을 읽지 않는다"의 범위 — node 전체인지 연결별인지 | dotnet node 전체 / jvm node 전체 / cpp 연결별 / node node 전체 | 확인 → 규칙은 `01-execution/05` §6이 이미 소유 | session 문서에 범위 문장·소유 링크 보강. 4언어 모두 이미 host 전체 적용 — 구현 수정 불필요 | 적용 완료 (D8) |
| G13 | session | 불일치 | 20 §4 (R40) | bind 시 target에 Actor 없고 Message Follow route 있으면 relay — 구현 흔적 없음(2~3언어). node는 ObjectGeneration 불일치도 `Unavailable` | dotnet 부분 / jvm 사유 미구분·relay 없음 / cpp 미발견 / node 미구현 | 확인 → 결정 필요 D9 | 불일치로 등록, relocation 주제에서 사실 확정 후 확정(D9) | 보류 — relocation 주제로 이월 (D9) |
| G14 | session | 오류 | 20 §3 (R21) | envelope 보존 값에 "binding token" — wire schema에는 `bindingGeneration`만 존재, token은 local handle | dotnet 상위집합 / cpp·node generation만 | 판정: 스펙 오류 J1 — 문서 정정 | envelope 목록을 "binding generation"으로 정정 | 자동 |
| G15 | session | 불일치 | 48 §2 (R56) | lane 정책 타입 재량의 판정 기준("의미 없는 조합 표현 불가")을 dotnet·node가 만족하지 않음 | dotnet flat enum / jvm 없음 / cpp variant ✔ / node flat | 판정: 불일치 J7 | 스펙 유지, dotnet·jvm·node 구현 수정 | 자동 |
| G16 | session | 불일치 | 20 §4·§4.1·§8 (R18·R29·R31·R32·R33·R36·R42·R49·R54·R59) | 단일 언어 이탈 묶음 — cpp: R18·R54 / jvm: R18·R35 / dotnet: R49·R48(b)·R11 로그 / node: R29·R31·R32·R33·R36·R42·R59 | 판정표 J8~J12, D10 | 판정: 불일치 — 구현 수정 | 스펙 유지, 각 언어 구현 수정 | 자동 |
| G17 | session | 오류 | 19 §8 (R16) | "node 경계를 넘는 record는 24·36·38·51뿐" — relocation control 42·43·44도 coordinator↔Session owner 사이를 넘음(4언어 동일) | 4언어 모두 42·43·44 전달 | 판정: 스펙 오류 J13 — 문서 정정 | §8을 "application record 4개 + relocation control 42·43·44"로 정정 | 자동 |
| G18 | channel-transport | 미지정 | 29 §4, 51 §5 | receive 독점 상한 3축(건수·byte·경과 시간)에 고정 값이 없음 — "무한하지 않다"와 "rotation 시작점이 이동한다"만 판정 가능 | dotnet 64건·4 MiB·1 ms / jvm 64건·1 MiB·2 ms / cpp 미보고 / node 64건·4 MiB·2 ms | 확인 — 3언어 확보, 값 발산 | 건수 64는 3언어 일치. byte(4 MiB vs 1 MiB)와 경과 시간(1 ms vs 2 ms)은 다름 → **결정 필요**: 값을 명시할지, 건수만 명시하고 나머지는 언어별 재량으로 둘지 | 적용 완료 — 02-channel-transport/05-transport-liveness.ko.md·01-execution/04-application-job-queue-and-backpressure.ko.md에 건수 64/회전 고정 + byte·경과 시간은 `**언어별 재량**`(관찰 결과 동일 근거·확인 기준 포함)으로 명시 |
| G19 | spot-actor | 모순 | 14 §6.4, 15 §2 | Actor 생성 상태 다이어그램의 세 번째 종결 leaf 이름이 두 문서에서 다름(`Failed` vs `Aborted`); 15의 산문은 Failed와 Abort를 별개 결과로 구분함 | jvm 공개 3-leaf + 내부 `FAILED` / node `failed` / dotnet·cpp 미보고 | 확인 — 2언어 `failed` 계열 | `Failed`로 통일하고 15의 `Aborted` 표기를 정정 | 적용 완료 — 03-spot-actor/04-actor-model.ko.md·05-spot-actor-membership.ko.md(다이어그램·step 7·검증 요구 3곳)와 00-foundation/02-glossary.ko.md의 creation-terminal-result 표를 `Failed`로 통일. relocation CAS abort를 가리키는 별개의 `Aborted`(05-spot-actor-membership.ko.md §8)는 다른 개념이라 유지 |
| G20 | location-relocation | 미지정 | 21 §6.4 vs 22 | 페이지 항목 상한 표현이 "1,024가 아니다"와 "2,048-key가 아니다"로 어긋남 | dotnet 두 값 실재(별개 메커니즘) / jvm 2,048-key batch 확인, 1,024 페이지 전용 상수 미특정 / node 페이지 1,024·1 MiB와 CAS batch 2,048은 별개 | 확인 — 모순 아님 | 두 값은 서로 다른 대상의 상한 → 각 값에 적용 대상을 명시(가이드 §6.2). 대장에서 종류를 `모순`→`미지정`으로 정정 | 적용 완료 — 05-location-relocation/02-location-store-redis.ko.md의 1,024(페이지)와 2,048(CAS batch unique key)이 서로 다른 대상임을 같은 문단에서 명시. 01-location-runtime.ko.md는 이미 각 수치에 범위가 붙어 있어 수정 불필요 |
| G21 | channel-transport | 미지정 | 45 §4 | `45` §4가 전제하는 connection projection API가 공개 계약 어디에도 없음 | dotnet 공개 API 없음(`ZLinkClientServerConnectionSnapshot`은 internal) / node 공개 snapshot API 없음 | 확인 중 — 2언어 확보 | 두 언어 모두 공개 API 없음. `45` §4가 전제한 API가 계약에 없으므로 그 전제 문장을 삭제하거나 계약을 추가해야 함 | 적용 완료 — 확인 결과 그 전제 문장은 45-merge 때 이미 02-channel-transport/02-channel-messaging.ko.md에 옮겨지지 않았음(topics/02-channel-transport/ledger-45-merge.md가 의도적으로 제외했다고 기록). 새 트리 본문에 해당 가정이 없어 추가 수정 불필요 |
| G22 | observability | 불일치 | 25 §4 (close_reason) | jvm이 `server_shutdown` 대신 `server_drain`을 쓰고, `idle_timeout`·`heartbeat_timeout` 종료는 `zlink.stream.connections.closed`에 계상하지 않음 | dotnet 일치 / **jvm 이탈** / cpp 일치 / node 일치 | 확인 | 스펙 유지, jvm 구현 수정 | 자동 |
| G23 | location-relocation | 미지정 | 용어집 | "Relocation Store"를 여러 문서가 쓰는데 용어집에 항목이 없음 | — | 후보 | 용어집에 항목 추가(가이드 §3.4 조건 충족) | 적용 완료 — 00-foundation/02-glossary.ko.md의 Location Store 항목 바로 뒤에 Relocation Store 항목 추가(05-location-relocation/03-relocation-store-redis.ko.md 내용 기반, 요약 표 형식 동일) |

## 리뷰 중 내려진 계약 판정 — R1

D1~D10과 별개로, 전체 게이트를 돌리다 드러난 발산에 대해 사용자가 내린 판정이다.

| R# | 무엇이 갈렸는가 | 판정 | 근거 |
|---|---|---|---|
| R1 | Session owner가 command 44를 자기가 설치한 seal과 대조할 때 쓰는 **matching seal identity**에 relocation coordinator identity를 넣는가. node 구현은 coordinator 5필드(ownerId·leaseGeneration·nodeRid·nodeGeneration·expectedAuthorityStoreVersion)를 포함한 11필드 JSON 배열을 썼고, 계약 테스트는 6필드만 기대했다 | **coordinator를 뺀다.** relocation identity(high·low), ActorId, ObjectGeneration, SessionRid, binding generation 여섯 값으로만 만든다 | [Session과 Actor binding §8.1](../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md)이 "Session owner는 다음 값만 검증한다"로 네 항목을 닫아 두었고, coordinator identity는 transport가 검증하는 wire fence다(G8). 같은 절이 "이 두 검증을 반복하거나 서로의 결과를 다시 판단하지 않는다"고 못박는다 |

적용: `packages/framework/src/runtime/foundation/service-stateful-wire-codec.ts`의
`serviceSessionRelocationIdentityKey`. 나머지 3언어도 같은 기준으로 대조해야 한다.

## 게이트 실행 중 발견 — R2

| R# | 무엇이 갈렸는가 | 판정 | 근거 |
|---|---|---|---|
| R2 | `zlink flow:` structured log 본문의 첫 key가 언어마다 다르다. dotnet은 `event_id=`, node는 `event=`로 시작한다 | **`event`가 맞다.** dotnet을 고친다 | [Message flow tracing](../../../framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md)의 "Structured log 대체 표기"가 key를 `event`, `phase`, `surface`, `kind`, `mesh`, … 로 고정한다. 기록 attribute의 이름(`event_id`, `message_kind` …)과는 별개 집합이다(R1 표 참조) |

관찰 근거 — ZoneWorld 샘플의 dotnet `zone-node-2.log`가 `zlink flow: event_id=zlink.message_flow`로
시작한다. node 구현은 `event`를 쓴다.

## 새로 발견 — G24: cpp hosted service가 coroutine 모델 밖에 있다

| 항목 | 내용 |
|---|---|
| 종류 | 미지정 — 공통 스펙이 hosted service의 실행 모델을 정하지 않는다 |
| 위치 | [cpp configuration과 host §4](../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md) |

Framework는 coroutine 기반인데 **cpp의 hosted service 진입점만 그 모델 밖에 있다.**

| 언어 | hosted service startup |
|---|---|
| dotnet | `async Task StartAsync(CancellationToken)` — `await`로 그대로 쓴다 |
| cpp | `virtual void start()` — **coroutine을 반환할 수 없다** |

그래서 비동기 준비가 필요한 cpp hosted service는 **샘플 코드가 `std::thread`를 직접 만들어
관리해야 한다.** ZoneWorld의 `zone_bootstrap_service_t`·`node_report_service_t`와
DeliveryDispatch의 dispatch service가 모두 그렇다. `test_cpp_framework_layout_contract`가
샘플 코드의 `.result()`를 금지하는 것도 이 구조가 만든 방어다 — 진입점이 coroutine이면
`co_await`를 쓰면 되고 blocking을 막을 이유가 없다.

**공통 스펙에 hosted service 항목 자체가 없었다.** 각 언어 interface 문서가 따로 정의해
모델이 갈렸다.

### 처리 — 사용자 지시(2026-08-26)로 framework를 고친다

1. **공통 스펙에 §22 Hosted service를 신설했다**([Framework API](../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md)).
   시작이 비동기 operation이라는 것, 시작 순서와 실패 처리, 정지 요청과 정지의 분리를 정한다.
   비동기 표현만 `**언어별 재량**`이고(`Task`·`task_t`·`Promise`·`CompletionStage`) 관찰
   결과는 고정이다 — 시작이 끝나야 다음이 시작하고, 실패하면 startup이 실패한다.
   §22 삽입으로 뒤 절을 재번호하고 깨진 anchor 3곳을 고쳤다.
2. **cpp `hosted_service_t::start`를 `task_t<void>` 반환으로 바꾼다.** 구현체 10개, 호출부
   1곳, cpp interface 문서의 선언을 함께 고친다.
3. 샘플이 `std::thread`로 비동기 준비를 흉내 내던 자리를 `co_await`로 바꾼다.

`test_cpp_framework_layout_contract`가 샘플의 `.result()`를 금지하는 것도 이 결함이 만든
방어였다 — framework 내부는 정작 `.result()`를 자유롭게 쓴다. 진입점이 coroutine이 되면
그 금지의 근거가 달라지므로 계약 테스트도 함께 재검토한다.

## Bingo 샘플 — 완료

### 스펙이 정하는 것과 정하지 않는 것

[Bingo 스펙 §11](../../../framework/doc/framework/common/sample/bingo/README.ko.md)은 완료
기준을 서술로 정하고 **시나리오 ID 체계가 없다**(ZoneWorld는 34개 ID 표가 있다). 그중
runner에 걸리는 항목은 이것뿐이다.

> Runner가 readiness, Redis lifecycle, **server-side evidence**와 두 완료 marker를 확인한다.

**어떤 증거를 어떤 문자열로 확인하는지는 정하지 않는다.** 그래서 세 언어가 갈렸다.

### 갈라진 것 — 같은 사실을 다른 문자열로, 또는 다른 사실을

| 확인 대상 | dotnet | node | cpp |
|---|---|---|---|
| player record 로드 | `bingo room: player record loaded. room=… actor=player-1, wins=0, losses=0` | `bingo-record fetched actor=player-1 wins=0 losses=0` | 확인하지 않음 |
| 결과 보고 | `bingo room: result reported. room=… won=True, wins=1, losses=0` | `bingo-record reported actor=player-1 wins=1 losses=0` | 확인하지 않음 |
| room leave | `bingo room: actor left. room=… actor=player-1` | `bingo-lifecycle room-leave actor=player-1` | 확인하지 않음 |
| entry destroy | `entry spot: actor destroy completed. actor=player-1` | `bingo-lifecycle entry-destroy-complete actor=player-1` | 확인하지 않음 |
| route readiness | 확인하지 않음 | 확인하지 않음 | `bingo route ready node=api-a mesh=room` |

로그 단언 수도 dotnet 16, cpp 17, node 5로 갈린다. **cpp만 route readiness를 보고
dotnet·node가 보는 업무 증거를 보지 않는다** — 같은 완료 기준을 서로 다른 증거로 검증한다.

추가 조사에서 java·kotlin은 더 나빴다 — **서버가 evidence 로그를 하나도 찍지 않고**,
runner는 `bingo=completed`와 `stream-inbound sample=Bingo` 두 줄만 본다. 그리고
`bingo-placement=completed`(스펙 §9가 요구하는 marker)는 dotnet만 출력한다.
node만 drain·node 교체(rolling replacement) 단계를 추가로 돌린다.

### 처리 — 스펙 §10.1 신설

[Bingo 스펙](../../../framework/doc/framework/common/sample/bingo/README.ko.md)에
**§10.1 Runner가 확인하는 server-side evidence**를 ko/en 양쪽에 넣었다. ZoneWorld에서
`topology=ready node=… zones=…`를 고정한 것과 같은 유형이다.

1. **문자열 고정** — readiness `bingo-ready kind=api-channel|peer-route|mesh-route …`,
   업무 evidence `bingo-record fetched|reported …`, `bingo-lifecycle room-leave|entry-leave|entry-destroy-complete|session-disconnect …`.
   "언어별 재량이 아니다, 문구를 바꾸려면 이 표를 먼저 바꾼다"를 명시했다.
2. **node 이름 고정** — `api-a, api-b, matchmaking, play-a, play-b, session-a, session-b`.
   cpp가 쓰던 `node=a`도 여기에 맞춘다.
3. **횟수 고정** — actor별 정확한 횟수. 0회 행(`bingo-record reported actor=observer`,
   `entry-destroy-complete actor=observer`)까지 표에 넣었다. dotnet의 집계식(`player record loaded` 2회)이
   아니라 node의 actor별 형식을 택했다 — 승패 tally가 dotnet의 `won=True` 플래그보다 검증력이 높다.
4. **대기 예산 고정** — `100 ms` 간격 최대 `300`회(30초), readiness와 evidence 동일 적용.
   dotnet은 대기 없이 사후 grep이었다.
5. **배치 독립성 보호** — node가 `session-b→player-1`로 노드를 고정해 세던 것을
   "두 Session node 로그를 합쳐 센다"로 바꿨다. 스펙 §9의 배치 독립성과 충돌하던 부분이다.
6. **기본 smoke 경계** — drain·node 교체·rolling relocation은 기본 샘플 성공 조건이 아니고
   §7.6 relocation 계약은 Config 11이 검증한다고 명시. node만 하던 단계다.
7. §11 완료 기준을 "§10.1 표의 모든 행을 문자열과 횟수까지 통과시킨다"로 교체했다.

구현 정렬은 5개 언어 각각 진행 중이다(java·kotlin은 서버 로깅 추가부터).

### 스펙 정정 — `api-channel` readiness 행 삭제

처음 §10.1을 쓸 때 cpp가 이미 찍던 `bingo play api channel ready node=a`를 그대로 표에 올렸다.
**그 행이 무슨 사실을 어떻게 증명하는지 확인하지 않은 것이 잘못이었다.** node 실행에서 드러났다.

- cpp는 이 행을 **합성 business request**로 증명한다 — `authenticate_player_req_t{"player-readiness"}`를
  500 ms 재시도 루프로 보내 accepted가 될 때까지 기다린다. readiness를 위해 가짜 player를
  샘플의 업무 경로에 밀어 넣는 것이다.
- node는 수동 신호(`clientServerRuntime.snapshot('bingo.api').isReady`)를 썼는데 켜지지 않는다.
  Play node는 이 채널의 `.client()`라 host state가 없고, `isReady`는 host state로 계산된다.
  **node 버그가 아니다.**
- 그리고 이 행은 커버리지를 더하지 않는다. `bingo-record fetched`/`reported`가 Play node에서
  API로의 실제 request 왕복이 성공한 뒤에만, 실제 승패 tally와 함께 찍힌다. 같은 사실을
  업무 traffic으로 이미 증명한다.

그래서 ko/en 양쪽에서 행을 지우고 "readiness를 위해 합성 request를 보내지 않는다"를 명시했다.
cpp의 probe hosted service와 node의 `observeApiChannel`을 함께 걷어냈고, cpp 계약 테스트
`test_cpp_framework_sample_parity.cpp`가 붙들고 있던 옛 문자열 `observer returned to entry spot`도
새 `bingo-lifecycle room-leave actor=`로 교체했다.

**교훈**: 스펙에 문자열을 고정할 때 기존 구현의 로그를 그대로 옮기지 말 것. 그 줄이 어떤
사실을 어떻게 증명하는지, 다른 행이 이미 증명하지는 않는지 먼저 읽어야 한다.

### 결과 — 5언어 전부 통과

| 언어 | 결과 |
|---|---|
| node | `bingo-placement=completed` (Claude가 직접 실행) |
| dotnet | `bingo-placement=completed` + 회귀 테스트 12/12 |
| cpp | `bingo-placement=completed` + `test_cpp_framework_sample_parity` 통과 |
| java | `bingo-placement=completed` |
| kotlin | `bingo-placement=completed` |

직접 재확인: `api-channel` 잔여 0건, 다섯 러너 모두 `bingo-placement=completed` 정확히 1회,
observer 4개 행(room-leave 1·entry-leave 1·entry-destroy-complete 0·record reported 0) 전부 검증.
cpp는 `for actor in player-1 player-2 observer` 루프로 덮는다.

**과정에서 배운 것**: codex 기본 sandbox는 loopback bind와 Docker를 막아 샘플을 못 돌린다.
네 언어 작업이 전부 "구현 완료, 실행 미검증"으로 끝났고 그중 node는 **실제로 깨져 있었다**
(§10.1의 `api-channel` 행 자체가 잘못이었다). 실행 검증 없는 구현 보고는 신뢰하지 않는다.
`codex exec -s danger-full-access`로 직접 기동하면 이 제약이 풀린다.

## TicTacToe 샘플 — 조사 결과 (스펙 상세화 대기)

Bingo와 **같은 병**이다. [TicTacToe 스펙 §11](../../../framework/doc/framework/common/sample/tictactoe/README.ko.md)도
문자열을 정하지 않고 서술만 한다 — "Runner가 Actor별 결과를 확인한다", "self-check와 server
evidence를 확인한다".

| 확인 대상 | dotnet | node | cpp | java | kotlin |
|---|---|---|---|---|---|
| actor destroy | `entry spot: actor destroy completed. actor=player-x` | 확인하지 않음 | 확인하지 않음 | `tictactoe actor destroy completed actor=player-x` | java와 같음 |
| leave 완료 | `actor: LeaveGameMsg completed. actor=player-x` | 확인하지 않음 | 확인하지 않음 | 확인하지 않음 | 확인하지 않음 |
| readiness | 확인하지 않음 | `spotPeerReady` (**framework 내부 문자열**) | 확인하지 않음 | `Started PlayProgram` / `Started ApiProgram` (**program boilerplate**) | java와 같음 |
| exact identity | `play stream: existing actor exact identity verified … actor=player-x` | 확인하지 않음 | 확인하지 않음 | 확인하지 않음 | 확인하지 않음 |
| client marker | 3종(`stream-inbound` + seq + Notify) + milestone·reconnect 2종 | 1종 | 1종 | — | — |

세 가지가 문제다.

1. **같은 사실을 다른 문자열로 쓴다** — destroy를 dotnet은 `entry spot: actor destroy completed.`,
   java·kotlin은 `tictactoe actor destroy completed`로 쓴다.
2. **readiness를 샘플이 아닌 남의 문자열로 확인한다** — node는 framework 내부 문자열
   `spotPeerReady`를, java·kotlin은 program boilerplate `Started PlayProgram`을 본다.
   둘 다 샘플이 소유하지 않는 문자열이라 framework·boilerplate가 바뀌면 조용히 깨진다.
3. **cpp는 client marker 한 줄만 본다** — server evidence를 전혀 확인하지 않는다.

처리는 Bingo와 같다 — 스펙에 §10.1 형식의 evidence 표(문자열·정확한 횟수·대기 예산·node 이름)를
넣고 다섯 언어를 맞춘다.

## DeliveryDispatch 샘플 — 조사 결과 (스펙 상세화 대기)

Bingo·TicTacToe와 같은 문자열 문제가 있고, **그보다 심한 것이 두 가지 더** 있다.

### 심각 — 스펙이 요구하는 시나리오를 아예 실행하지 않는다

이건 "검증 문자열이 갈렸다"가 아니라 **스펙 요구사항이 4~5개 언어에서 실행조차 되지 않는다**는
문제다.

| 스펙 요구 | 실행 상태 |
|---|---|
| §9-6 늦게 도착한 `CourierDecisionMsg`가 효과 없음 | **node만 실제로 시험한다.** node는 courier-b가 수락한 *뒤에* courier-a 결정을 보내고, 서버가 `ignored stale decision delivery=… courier=courier-a attempt=1`을 남기는지 확인한다(`dispatch-worker.ts:94`). **dotnet·cpp·java·kotlin은 courier-a가 결정을 아예 보내지 않는다** — 늦은 메시지가 없으니 "재수락 안 함" 보장이 시험되지 않는다 |
| §9-9 server evidence 직접 검증 | **dotnet은 `/self-check/assert` 엔드포인트가 서버에 존재하지 않는다.** cpp·java·kotlin은 있고 러너가 확인한다. node는 있지만 **하네스가 확인하지 않는다** |
| §9-7 후보 없는 delivery가 `Failed`에 정확히 한 번 도달 | **다섯 언어 모두 확인하지 않는다.** 후보 없는 delivery를 만드는 시나리오 자체가 없다 |
| §9-8 response·push에 ActorRef·NodeRid·session route가 없음 | 다섯 언어 모두 런타임 확인 없음. DTO 모양으로만 보장 |

### 같은 문자열이 다른 순간을 증명한다

`topology=ready` — **dotnet은 두 courier bind가 끝난 *뒤*에** client가 출력하고
(`DeliveryDispatchClientScenario.cs:37`), **kotlin은 아무것도 연결하기 *전*에** 출력한다
(`Program.kt:45`). java는 client가 출력하지 않고 **bash 러너가 client 시작 전에 스스로 echo한다**
(`run_sample.sh:283`) — 아무것도 증명하지 않는다. 같은 리터럴이 세 언어에서 세 가지 의미다.

### 샘플이 소유하지 않는 문자열에 의존

- **java·kotlin**: `ZLINK_FRAMEWORK_READY`·`ZLINK_FRAMEWORK_PEER_READY`·`ZLINK_FRAMEWORK_TERMINATION`
  (`ZLinkFrameworkRuntime.java:533`, `ZLinkJavaRawMeshNode.java:6836`, `ZLinkFrameworkLifecycle.java:257`).
  `runner-common.sh`를 통해 **모든 java/kotlin 샘플이 공유**하므로 framework 로그 형식이 바뀌면
  전 샘플 러너가 동시에, 조용히 깨진다.
- **cpp**: `message flow` — framework 진단 tracer가 찍는 문자열이다
  (`message_flow_tracer.hpp:438`). TicTacToe·ShoppingMall·Bingo·SupportChat도 같이 쓴다.

### 언어별로만 존재하는 검증

dotnet은 서버 로그 5행(tracking·customer-gateway·courier-session)을 보는데 **다른 언어는 하나도
안 본다**. java만 `courier-bind-relayed=<id>`를 본다. node만 role별 `deliverydispatch-route-ready`를
본다. cpp만 `/ready?targetRid=` HTTP probe로 actor 수용 준비를 확인한다(**러너가 만들어 보내는
합성 요청** — Bingo에서 걷어낸 것과 같은 범주다. 다른 언어는 TCP connect나 로그로 공짜로 얻는다).
java·kotlin만 프로세스를 띄우기 전에 `rg` 정적 소스 가드레일을 돌린다.

### 대기 예산

| | 포트 | HTTP | 로그 marker |
|---|---|---|---|
| dotnet | 60초 | 12초 | 16초 |
| cpp | 15초 | 12초 | 없음(사후 grep) |
| node | 30초 | 30초 | 30초 |
| java·kotlin | 60초 | 60초 | 60초 |

신호 종류마다 예산이 다르고 언어마다 또 다르다. 고정 sleep을 readiness 대신 쓰는 곳은 **없다**
— 이 항목만은 다섯 언어가 스펙(§10, "고정 sleep 금지")을 지킨다.

### 완료 marker

`deliverydispatch=completed`는 스펙이 유일하게 문자열까지 고정한 것이고 다섯 언어가 일치한다.
다만 **node는 출력만 하고 확인하지 않는다** — 완료 판정을 browser의
`window.__zlinkSampleResult.status === 'passed'`로만 한다. 내일 client가 이 문자열을 안 찍어도
node 러너는 모른다.

## GameQuest 샘플 — 조사 결과 (스펙 상세화 대기)

**모두 직접 재확인한 사실이다.**

### 버그 3건

| 문제 | 근거 |
|---|---|
| **dotnet은 스펙이 정한 완료 marker를 아예 출력하지 않는다** | `gamequest=completed`가 dotnet GameQuest 트리 전체에 **0건**. `gamequest-server-evidence=completed`로 대체해 버렸다. 스펙 §10이 문자열까지 고정한 그 marker다 |
| **dotnet `.ps1`과 `.sh`의 대기 예산이 20배 다르다** | 같은 readiness 검사에 `.ps1`은 `-Attempts 30`(×100 ms = **3초**), `.sh`는 `seq 1 600`(×0.1초 = **60초**) |
| **dotnet `grep -q`가 AND가 아니라 OR로 동작한다** | `run_sample.sh:252`가 `grep -q "gamequest api event routed" api-a.log api-b.log` — `grep -q`는 **어느 한 파일**만 맞아도 0을 반환한다. 바로 위(250·251행)와 아래(253·254행)는 파일별로 나눠 쓴 올바른 형태라 의도는 AND가 분명하다. 한 노드만 라우팅해도 통과한다 |

### 스펙 요구인데 다섯 언어 모두 확인하지 않음

- **§9-9 Ready owner 프로세스 종료 → 다음 호출이 `Unavailable`, 자동 대체 없음.** §11 완료 기준에도
  "Ready owner 장애를 crash failover로 표시하지 않고 Unavailable 경계를 확인한다"고 적혀 있는데
  **다섯 러너 어디에도 이 단계가 없다.**

### 언어별 검증 범위 편차가 극단적

node 러너는 **로그 단언이 0건**이다(`sample-runner.mjs` 전체 56줄, `/health` 폴링 뒤 browser에
넘긴다). kotlin은 shell 단언 2건. 반면 java는 **rehydrate 재실행 단계**(클라이언트를 두 번째로
띄워 전용 로그에 marker 확인)와 **scale-out 단계**, 그리고 프로세스를 띄우기도 전에 도는
**`rg` 정적 소스 게이트**까지 있다. kotlin 러너는 java에서 그 단계들을 **지운** 사본이다.

주의: node·kotlin이 "덜 검증"하는 건 아니다. 업무 단언은 클라이언트 안 `ensure`/`check`에 있고
실패하면 프로세스가 비정상 종료해 `set -euo pipefail`로 러너가 죽는다. 다만 **러너가 독립적으로
관찰하는 사실**은 확연히 적다.

### 같은 사실 다른 문자열 / 샘플이 소유하지 않는 문자열

- reconcile 증거: dotnet `QuestReconciled` vs java `"eventType":"QuestProgressReconciledEvent"`.
- mesh 라우팅 증거: **두 언어도 같은 문자열을 쓰지 않는다** — dotnet
  `surface=stream kind=request packet=JoinSessionReq`(framework의 `TestHostMessageFlowListener.cs:37`),
  cpp `message flow`(`message_flow_tracer.hpp:438`), java `surface=spot kind=send…`(framework의
  Spec-26 trace 포맷 `ZLinkTraceFormat.java`), node·kotlin은 확인 안 함. **전부 framework 소유
  문자열이다.**
- java·kotlin은 `ZLINK_FRAMEWORK_READY`·`ZLINK_FRAMEWORK_PEER_READY`에 의존.

### 합성 probe

java·node·kotlin이 `POST /self-check/owner/player-alice/close`,
`/self-check/gameplay/kill-without-publish/{id}`, `/self-check/projection/{…}/delete|rebuild`로
**상태를 인위적으로 만들어** 증거를 생산한다. cpp만 합성 probe가 없다.

## ShoppingMall 샘플 — 조사 결과 (스펙 상세화 대기)

### 스펙을 정면으로 어기는 것 — 5개 중 4개 언어

스펙 §9는 runner 전용 hook(pending 매핑, 중단 fixture, server evidence)을 **Client 프로세스
바깥에서** 호출하라고 명시한다. Client는 CommerceApi의 공개 order API만 쓴다.

- **cpp·node·java·kotlin은 `/self-check/*`를 Client 안에서 호출한다.** dotnet만 shell 러너에서
  Client 시작 전에 `curl`로 처리해 스펙대로다.
- **kotlin은 더 나아간다 — Client가 HTTP를 아예 쓰지 않는다.** 내부 RouteMesh channel API로
  CommerceApi에 직접 말한다(`channels.requestToChannel(...)`). 스펙이 말하는 "공개 order API 표면"이
  **전혀 시험되지 않는다.**

### 스펙 요구인데 다섯 언어 모두 확인하지 않음

- **§9.2-11 계획된 relocation 단계.** 다섯 언어 모두 서버 부트스트랩에 `RelocationStore`·
  `recreateOnRelocation`을 배선해 놓았지만, **어느 러너도 노드를 죽이거나 relocation을 강제하지
  않고 "stream replay·`ReserveInventoryReq` 재전송 없음"을 단언하지 않는다.**

### 그 밖

- **cpp ShoppingMall에는 `run_sample.ps1`이 아예 없다**(직접 확인). dotnet·java·kotlin은 있다.
- **kotlin은 `shoppingmall=completed`를 출력하지만 러너가 확인하지 않는다.** node도 출력만 하고
  프로세스 종료 코드로만 판정한다.
- dotnet의 `/self-check/assert` probe는 **하드코딩된 order ID**(`order-0001`…)를 보낸다 — 실제
  실행이 만든 ID와 대조하지 않는다.
- §10.6이 요구하는 "runner placement marker"가 세 가지로 갈리고(`PASS ShoppingMall.Cpp`,
  `PASS ShoppingMall.Ts`, 없음×3) 표기 주체도 shell/client로 갈린다.
- "workflow가 order를 시작했다"를 dotnet은 **OR**로, cpp·kotlin은 **AND**로 확인하고 java는
  확인하지 않는다. cpp만 `.*spot=` 정규식으로 더 엄격하다.
- 상태 폴링 예산: java 30초, dotnet·cpp 8초, node 4초, kotlin 12초(서버는 8초).

## SupportChat 샘플 — 조사 결과 (스펙 상세화 대기)

### 가장 심각 — cpp의 단언 8건이 아무것도 증명하지 않는다

`Client/supportchat_client_scenario.hpp:36-48`을 직접 확인했다. `run_server_self_check()`와
`run_stream_conversation()`을 부른 **뒤에 8개 marker를 한 블록에서 무조건 출력**한다.

```
run_server_self_check (support_http_url);
run_stream_conversation (session_stream_endpoint);
std::cout << "supportchat authentication=verified" ...   // 8줄이 조건 없이 연달아
```

위에서 예외가 나면 한 줄도 안 찍히고, 러너는 그 전에 client 종료 코드로 이미 죽는다
(`run_sample.sh:187-190`). **따라서 러너의 `grep -q` 8건은 "프로세스가 0으로 끝났다"는 사실
하나를 여덟 번 확인하는 것과 같다.** 어느 사실이 실제로 일어났는지 구분하지 못한다.

### 두 번째 — cpp의 self-check는 실제 경로를 우회한다

`POST /self-check/assert`가 서버에서 `supportchat_server_story_t::run()`을 돌리는데
(`Server/Support/main.cpp:834-932`), 이건 **`Conversation` 도메인 객체를 직접 만들어 굴리는
in-process 전용 이야기**다 — actor Spot도, Session relay도, wire codec도 타지 않는다. 그러고는
`one-agent-many-conversations=verified` 같은 **미리 정해진 문자열**을 돌려준다.
같은 사실들은 뒤이어 `run_stream_conversation()`이 실제 stream으로 다시 시험한다.
**Bingo에서 걷어낸 합성 probe와 정확히 같은 구조이고, 실제 증거와 중복이기까지 하다.**

### 버그 — kotlin `.ps1`이 잘못된 파일을 본다

`run_sample.ps1:173`은 `status=WaitingForAgent`를 **support.log**에서 찾고,
`run_sample.sh:142`는 같은 사실을 **api.log**에서 찾는다. 두 파일을 직접 확인했다.
같은 샘플의 두 러너가 같은 사실을 다른 파일에서 찾고 있다.

### 그 밖

- **cpp와 java에는 `run_sample.ps1`이 없다.** dotnet·kotlin만 있다.
- **node와 java는 완료 marker `supportchat=completed`를 확인하지 않는다** — 출력만 한다.
- dotnet `.ps1`의 `Assert-SampleLogContains`는 **재시도 없는 단발 검사**인데 `.sh`의 같은 검사는
  0.2초 × 50회(10초) 폴링이다. `.ps1`이 경합에 그대로 노출된다.
- 상태 전이 증거(`status=Active` 등) 확인 개수: dotnet 6, kotlin `.sh` 6, kotlin `.ps1` 5(그중 1건
  위 버그), java 4, **cpp 0**.
- `supportchat-closed-typing-ignore=verified`가 cpp만 **하이픈 대신 공백**
  (`supportchat closed-typing-ignore=verified`).
- framework 소유 문자열 의존은 다른 샘플과 동일 — java·kotlin `ZLINK_FRAMEWORK_READY`·
  `ZLINK_FRAMEWORK_PEER_READY`·`zlink flow: event_id=zlink.message_flow`, cpp `message flow`.
- 대기 예산 10초~60초, dotnet은 `.sh`와 `.ps1`이 6배 차이.

### 내 단서가 틀렸다 — kotlin `api-channel`은 합성 probe가 아니다

조사 전에 나는 kotlin SupportChat의 `api-channel`이 Bingo에서 지운 것과 같은 종류일 것이라고
단서를 줬다. **틀렸다.** `wait_port api-channel`은 그냥 TCP connect-and-close이고
(`runner-common.sh:269-322`), dotnet의 `wait_port api-mesh`, node의 `waitTcp(apiChannelEndpoint)`와
구조가 같은 **수동 신호**다. 지울 게 아니라 다른 언어와 표기만 맞추면 된다.
SupportChat의 진짜 합성 probe는 위의 cpp `/self-check/assert`다.

## 고정 RID + Object role — 언어별 framework 계약 불일치 (판정 필요)

TicTacToe 통일 과정에서 드러났다. **같은 설정을 cpp·java는 받아들이고 dotnet만 거부한다.**

- **cpp**: `game_spot.set_routing_id(routing_id_t::from("tictactoe-play-a"))`를 Object role mesh에
  걸고 **정상 동작한다**(`play_server_host_factory.hpp:56`). TicTacToe cpp 러너는 통과한다.
- **java**: 실행 로그의 peer 이름이 `tictactoe-play-a`/`tictactoe-play-b`로, 마찬가지로 고정
  RID를 쓴다.
- **dotnet**: 같은 조합을 **설정 검증에서 거부**한다
  (`ZLinkSpotRegistrationValidator.cs:48`) —
  `HasExplicitRoutingId && (automatic || ObjectRoleSelected)` → `ZLinkConfigurationException`
  `"MeshNode 'tictactoe' cannot use a fixed routing ID with automatic discovery or an Object role."`

이 때문에 dotnet TicTacToe가 기동조차 못 한다.

### 왜 여기서 걸렸나

내가 §10.1에 "`kind=peer-route` 행은 **표에 적힌 그 peer가 ready인지** 확인한다"를 넣었는데,
dotnet의 공개 API로는 그 요구를 만족할 수단이 없다. `ZLinkPeerStatus`는 `NodeRid`·`State`·
`UnavailableReason`만 노출하고 **endpoint를 주지 않으므로**, RID가 자동 생성되면 어느 peer가
상대인지 식별할 방법이 없다. cpp·java는 RID를 고정해 그 문제를 피했고 dotnet은 그 길이 막혀 있다.

### 판정 — 스펙에 이미 답이 있었다

규칙이 **문서에 이미 있다.** 두 곳이 같은 말을 한다.

- `languages/dotnet/interfaces/03-configuration-topology.ko.md:356` — "Fixed `SetRoutingId(...)`는
  **object role과 Store descriptor가 없는 manual topology에서만** 허용한다."
- `languages/kotlin/interfaces/configuration-host.ko.md:202` — "Fixed RID는 **object role이나
  automatic Store descriptor가 없는 manual topology에서만** 사용할 수 있다."

**kotlin 문서에도 적혀 있는데 kotlin은 강제하지 않는다.** framework 소스에서 이 검증을 실제로
하는 것은 dotnet 하나뿐이다 — java·node에서 검색되는 "fixed routing ID" 문자열은 전부 **channel
publisher용 다른 규칙**이고 spot-node mesh의 object role 조합과 무관하다.

| | 문서 규칙 | 검증 구현 | 샘플이 위반 |
|---|---|---|---|
| dotnet | 있음 | **있음** | 못 함(거부당함) |
| kotlin | 있음 | 없음 | 확인 필요 |
| cpp | 확인 필요 | 없음 | **있음** — `set_routing_id`를 object role mesh에 |
| java | 확인 필요 | 없음 | **있음** — `tictactoe-play-a` |
| node | 확인 필요 | 없음 | 확인 필요 |

**dotnet이 스펙대로이고 나머지가 검증을 빠뜨렸다.** 그 틈으로 TicTacToe 샘플이 문서가 금지한
설정을 쓰고 있었다.

### 처리 (1) — 샘플 스펙: 완료

§10.1의 "표에 적힌 그 peer가 ready인지 확인한다"를 **내렸다.** 고정 RID가 금지된 이상
`ZLinkPeerStatus`에 endpoint가 없어 자동 RID인 peer를 이름으로 식별할 공개 수단이 없다.
내가 만족 불가능한 요구를 스펙에 넣은 것이었다. `peer=` 값은 "누구를 기대하는지 읽는 사람에게
알리는 표시"로 남기고, 판정은 "ready인 peer가 하나 이상"으로 되돌렸다.

dotnet TicTacToe는 고정 RID를 걷어내고 러너 통과를 확인했다(`tictactoe-placement=completed`).

### 처리 (2) — framework: 미착수, 캠페인 범위 밖

cpp·java·kotlin·node에 dotnet과 같은 검증이 없다. 넣으면 **지금 그 설정을 쓰는 샘플들이 기동
불가가 된다**(cpp·java TicTacToe가 확실히 해당). 검증 추가와 샘플 정리를 함께 해야 하므로
별도 항목으로 둔다.

**교훈**: 스펙에 새 요구를 넣기 전에 **그 언어의 공개 API로 만족 가능한지** 확인할 것. 이번에는
`ZLinkPeerStatus`가 endpoint를 노출하지 않는다는 사실 하나로 요구 전체가 불가능해졌다.

## TicTacToe java/kotlin — 고정 sleep이 덮고 있던 것 (미해결, 판단 필요)

TicTacToe 구현 정렬에서 dotnet·cpp·node는 `tictactoe-placement=completed`로 통과했고
**java·kotlin만 실패**한다. 고정 `sleep 30`을 걷어낸 뒤 client의 첫 호출이 timeout된다.

### 1차 원인은 내 스펙이었다 — 고쳤다

세 언어 모두 `tictactoe-ready kind=http`를 **HTTP가 listen하자마자** 찍는다
(`ApiServer.cs:56`, `host_support.hpp:239`). 라우팅에 대해 아무것도 증명하지 않는다. 즉
dotnet·cpp·node는 **타이밍 운으로 통과**하고 있었다. DeliveryDispatch에 `kind=actor-route`,
ShoppingMall에 `kind=object-route` 행을 넣으면서 **TicTacToe에만 같은 행을 빠뜨린 것**이
내 잘못이다. 세 번째 readiness 행 `kind=spot-route`를 추가하고 다섯 언어에 구현했다.
다섯 언어 모두 route mesh 준비를 관찰한 자리에서 emit하는 것을 diff로 직접 확인했다.

### 그런데 java는 여전히 실패한다 — 더 깊은 것이 있다

실행 로그의 시간선이다(`run-ttt-java.log`).

| 시각 | 사건 |
|---|---|
| 02:13:47.282 | api-a에 `PEER_READY … peer=tictactoe-play-a` |
| 02:13:47.332 | api-a가 `tictactoe-ready kind=spot-route node=api-a mesh=tictactoe` 출력 — **신호는 스펙대로 동작했다** |
| 02:14:00.21 | client 인증·observer 구독 성공 |
| 02:14:00.2x | 다음 호출이 `TimeoutException` |

**같은 peer(`tictactoe-play-a`)에 대한 `PEER_READY`가 api-a 로그에 15회, 약 2.5초 간격으로
반복된다.** 캡처된 출력에는 그 사이에 lost/disconnect 줄이 없다.

**확인한 것**: readiness 신호 자체는 스펙대로 동작했고, 그 시점에 play-a는 실제로 READY였다.
**확인하지 못한 것**: 이 반복이 재연결(flapping)인지 주기적 재공지인지, 그리고 그것이 timeout의
원인인지. lost 이벤트가 INFO로 안 찍히는 것일 수도 있다.

### 판단 필요

이 증상이 flapping이라면 **java·kotlin이 애초에 `sleep 30`을 두었던 진짜 이유**가 "route 수렴이
느려서"가 아니라 **"peer가 안정될 때까지 기다리려고"**였다는 뜻이다. 그렇다면 sample 스펙으로
닫을 수 없고 framework 영역이다. abort 건과 같은 성격이라 사용자 판단을 받는다.

**임시로 sleep을 되살리지 않는다** — 그건 스펙 §10 3단계가 금지하는 것이고, 무엇보다 이 증상을
다시 숨긴다.

## ZoneWorld 샘플 — 언어별 구현 불일치

시나리오 ID 집합은 세 언어가 **34개 완전히 일치**하고, 각 ID의 전제·행동·단언은
[ZoneWorld 샘플 스펙](../../../framework/doc/framework/common/sample/zoneworld/README.ko.md)이
소유한다. 그 문서가 "언어별 runner가 일부 ID를 runner-driven으로 구현할 수는 있으나 ID의
전제·행동·단언 의미는 바꾸지 않는다"고 정한다. 그런데 실행 범위와 설정이 갈려 있다.

| 항목 | dotnet | node | cpp | 판정 |
|---|---|---|---|---|
| 브라우저(Playwright) 단계 | 있었음 — `run_sample.sh`에 playwright 참조 31곳 | 없음 | 없음 | **정리함** — 기본값을 끔으로 바꾸고 `--browser-smoke` 옵트인으로 남겼다. 샘플 검증은 client connector 경로만 돈다 |
| 오케스트레이션 | bash 1,060줄 | bash 10줄 + `Runner/sample-runner.mjs` 790줄 | bash 601줄 | 미정리 — 같은 34개를 검증하는데 구조와 분량이 제각각이다 |
| owner lease | Ops만 TTL 3초로 재정의 | Ops·Gateway·ZoneNode 전부 TTL 3초 | 재정의 없음 | **정리함 — 재정의를 전부 걷어냈다.** ZoneWorld 스펙은 report TTL 15초(§2.2, ZW-C3)만 정하고 owner lease 재정의를 요구하지 않는다. 스펙대로면 [Location runtime §5](../../../framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md)의 기본값(TTL 15초·갱신 5초·timeout 3초·fencing margin 5초)을 그대로 쓴다. cpp가 원래 맞았다 |
| ZW-G4 재시작 config | `zone-node-crash-replacement`(`allowEmptyZoneSet=true`) — zone 0개로 ready | `topology=ready node=… zones=`(빈 zone)을 기다린다 | 평소 config로 재기동 | **cpp가 이탈** — [ZoneWorld 스펙 §7.5](../../../framework/doc/framework/common/sample/zoneworld/README.ko.md)는 crash replacement를 "같은 NodeId로 새 process를 시작해 **새 object를 수용할 수 있게 되는 것**"으로 정의하고 "**이전 Ready owner가 소유하던 object의 자동 복원·재생성이 아니다**"라고 못박는다. dotnet·node가 맞고, 평소 config로 띄우면 죽은 owner의 zone을 다시 claim하려 한다 |
| `.sln` | 7개 샘플 중 3개만 있었음 | — | — | **정리함** — GameQuest·ShoppingMall·SupportChat·ZoneWorld에 추가 |

또한 dotnet Ops 코드 주석이 "the documented 30-second defaults"라고 하지만 스펙이 정한
기본값은 **`OwnerLeaseTtl` 15초**다([Location runtime §5](../../../framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md)).
framework 구현 기본값은 4언어 모두 스펙과 일치하므로, 어긋난 것은 주석뿐이다.

## 구현 부채 — 처리 결과

작성 시점에는 "이 캠페인에서 코드는 고치지 않는다"는 전제였다. 이후 사용자 지시로
**배치 1~4에서 실제로 고쳤고**, 4언어 단위·계약 테스트와 언어간 통신 smoke, 7샘플까지
그린이다. 아래는 확인한 최종 상태다.

| 출처 | 언어 | 할 일 | 상태 |
|---|---|---|---|
| D4 | dotnet, jvm, node | 같은 host에서 같은 session type을 둘 이상의 node에 등록하면 startup 실패 | **완료** — 3언어 모두 검사와 회귀 테스트 있음 |
| D5 | 4언어 | `SessionReplacementCallbackTimeout`(기본 30,000 ms) 도입, 하드코딩 제거 | **완료** — 4언어 모두 option 노출 |
| D6 | dotnet, jvm, node | Actor factory 없음의 오류를 `NotFound`로 | **완료** |
| D7 | dotnet, node, cpp | `boundSessionReplaced(51)`의 별도 재시도 제거 | **완료** |
| D3 | dotnet, cpp, node | late·duplicate command 44에 `late_session_route_update` Warning | **완료** — 3언어 모두 기록 |
| D10 | dotnet | `MaxMessageSize` 초과 로그에 `EMSGSIZE` | **완료** |
| D1 | cpp | eager coroutine 첫 turn이 session thread에서 시작하는 동작 정정 | **완료** — Actor relay 제출을 별도 실행기로 전환 |
| G15 | dotnet, jvm, node | lane 정책 타입이 의미 없는 조합을 표현하지 못하게 | **완료** — jvm에 `ZLinkExecutionLanePolicy`와 컴파일 검증 테스트 추가 |
| G22 | jvm | `close_reason`을 `server_shutdown`으로, idle·heartbeat timeout을 counter에 계상 | **완료** |
| R1 | node, jvm | matching seal identity에서 coordinator 제거 | **완료** |

### 남은 것 — 0건

| 출처 | 언어 | 결과 |
|---|---|---|
| R2 | dotnet | **완료** — `zlink flow:` 본문 첫 key를 `event`로 바꿨다. telemetry attribute(`event_id`)와 본문 축약 key(`event`)는 다른 집합이며 본문의 첫 필드만 어긋나 있었다. 본문 문자열을 검증하던 테스트 3곳도 함께 맞췄고 `Tag("event_id")`(attribute 조회)는 그대로 두었다 |
| ZW-G4 | cpp | **이탈 아님 — 판정 정정.** cpp ZoneNode는 startup에서 zone을 claim하지 않고 factory만 등록한 뒤 `zoneworld-role-ready`를 알린다(`Server/ZoneNode/main.cpp`). 이전 owner의 zone을 자동 복원하지 않으므로 §7.5에 이미 맞다. dotnet이 `allowEmptyZoneSet` 플래그를 따로 둔 것은 dotnet만 `BotSpawner`가 startup에서 zone 2개를 확보하는 구조이기 때문이다 |

### 더 큰 구조 차이 — zone을 언제 확보하는가

ZW-G4를 파다가 드러났다. 세 언어의 ZoneNode가 **zone Spot을 만드는 시점**이 다르다.

| 언어 | startup에서 zone 확보 | 재시도와 실패 처리 | crash 교체 시 |
|---|---|---|---|
| dotnet | 한다 — `BotSpawner` | 120회 × 250ms, 초과하면 예외로 프로세스 종료 | `allowEmptyZoneSet=true` |
| java | 한다 — `ZoneBootstrap` | `attempt >= 119`, `IllegalStateException` | `allowsEmptyZoneSet()` |
| kotlin | 한다 — `ZoneOperations` | `attempt < 119`, `check` 실패 | `allowsEmptyZoneSet()` |
| node | 한다 — `bootstrapZones` | — | `bootstrapZones: false` |
| cpp | **안 한다** — factory만 등록하고 `zoneworld-role-ready`. zone Spot은 첫 요청 때 만들어진다 | — | 별도 처리 불필요 |

**4:1로 cpp만 다르다.** dotnet·java·kotlin은 재시도 횟수(120)와 조기 탈출 조건(`attempt >= 8`)
까지 같아 한 설계를 포팅한 것이 분명하다. 스펙은 "언어별 runner가 일부 ID를 runner-driven으로
구현할 수는 있으나 ID의 전제·행동·단언 의미는 바꾸지 않는다"고 허용하지만, 이 차이는
관측 의미에 닿는다.

- **dotnet만 "zone 확보 실패 → 30초 뒤 프로세스 종료"** 경로를 갖는다. 오늘 실패의 상당수가
  이 경로였다(`topology=ready`를 못 찍음).
- `ZW-C1`은 "두 ZoneNode의 Registered·Connected 각각 정확"을 단언한다. cpp에서는 zone을
  하나도 갖지 않은 노드도 Registered·Connected가 참일 수 있어 같은 문장이 다른 상태를
  통과시킨다.

**판정이 필요하다** — 세 언어가 zone 확보 시점을 통일해야 하는지, 아니면 스펙이 그 시점을
명시해야 하는지. 이 캠페인에서 정하지 않고 올린다.

**ZW-G4 판정을 두 번 뒤집었다.** 처음엔 dotnet이 이탈이라 보고 cpp 방식으로 바꿨다가, §7.5를
끝까지 읽고 dotnet이 맞음을 확인해 되돌렸고, 다시 cpp를 이탈로 올렸다가 cpp 구현을 직접 읽고
아님을 확인했다. **문서의 요약 행만 보고 판단하지 말 것** — 그 행이 가리키는 절과 각 언어의
실제 코드를 함께 읽어야 한다.

G16(단일 언어 이탈 묶음, 판정표 J8~J12)은 session 주제 대조 때 기록한 것으로, 위 항목들과
겹치는 부분이 처리되면서 대부분 해소됐다. 남은 항목은 판정표에서 개별 확인이 필요하다.
