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

## 구현 부채 — 이 캠페인이 만들고 넘기는 목록

문서를 확정한 결과 "지금 구현이 계약과 다르다"가 확정된 항목이다. **이 캠페인에서는 코드를
고치지 않는다.** 별도 구현 작업으로 넘긴다.

| 출처 | 언어 | 할 일 |
|---|---|---|
| D4 | dotnet, jvm, node | 같은 host 안에서 같은 session type을 둘 이상의 node에 등록하면 startup 실패시키는 검사 추가 |
| D5 | jvm, cpp | 교체 callback deadline 5초 하드코딩 제거, `SessionReplacementCallbackTimeout`(기본 30,000 ms) 도입 |
| D5 | dotnet, node | 같은 option 이름으로 노출(현재 30초는 `DefaultRequestTimeout` 등 다른 값에 얹혀 있음) |
| D6 | dotnet, jvm, node | Actor factory 없음의 오류를 `NotFound`로 변경 |
| D7 | dotnet, node, cpp | `boundSessionReplaced(51)` 전송에 얹은 별도 재시도 제거. send timeout·`DeadlineExceeded` 일반 규칙만 사용 |
| D3 | dotnet, cpp, node | late·duplicate command 44에 `late_session_route_update` Warning 기록 |
| D10 | dotnet | `MaxMessageSize` 초과 로그에 `EMSGSIZE` 포함 |
| D1 | cpp | eager coroutine 첫 turn이 session thread에서 시작하는 동작 정정 |
| G15 | dotnet, jvm, node | lane 정책 타입이 의미 없는 조합을 표현할 수 없게 변경 |
| G22 | jvm | `close_reason`을 `server_drain`에서 `server_shutdown`으로, `idle_timeout`·`heartbeat_timeout` 종료를 `zlink.stream.connections.closed`에 계상 |
| ZW-G4 | cpp | crash 교체 노드를 평소 config로 띄우지 않는다. ZoneWorld 스펙 7.5대로 이전 owner의 zone을 되찾지 않는 재기동으로 바꾼다(dotnet의 `allowEmptyZoneSet`, node의 빈 zone 대기가 기준) |
| R2 | dotnet | `zlink flow:` 본문 첫 key를 `event_id`에서 `event`로 바꾼다 |
| G16 | 언어별 | 단일 언어 이탈 묶음 — 판정표 J8~J12 참조 |

D9(bind 시 Message Follow relay)는 4언어 어디에도 구현이 없어 계약 의도 자체를 재확인해야
하므로 이 목록에 넣지 않고 relocation 주제로 이월한다.
