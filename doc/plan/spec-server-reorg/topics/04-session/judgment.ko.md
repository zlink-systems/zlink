# session 주제 — gap 판정표

> 입력: [gap-session-dotnet](gap-dotnet.md) · [gap-session-cpp](gap-cpp.md) ·
> [gap-session-node](gap-node.md) · [gap-session-jvm](gap-jvm.md).
> 검토 기준 commit `f1a2f416f6`. 판정 규칙은 [spec-gap 대장](../../spec-gap.ko.md) 머리말.

## 0. 재작성 오류 여부

네 언어의 `불일치` 항목을 옛 문서(19·20·48)와 대조했다. 새 문서가 옛 문서와 다르게 쓴 규칙은
없다 — 모든 불일치는 옛 스펙 시점부터 있던 것이다. 재작성으로 생긴 문서 오류: **0건**.

## 1. 자동 판정 (사용자 결정 불필요)

| # | 항목 | dotnet | jvm | cpp | node | 판정 | 처리 |
|---|---|---|---|---|---|---|---|
| J1 | R21 envelope에 "binding token" | 상위집합 | generation만 | generation만 | generation만 | **스펙 오류** — wire schema(`service-wire-v1.schema.json`)에 `bindingGeneration`만 있고 token 없음. token은 local handle | session 문서 §5 envelope 목록을 "binding generation"으로 정정 |
| J2 | G2(b) stale fence의 공개 `ErrorKind` | `Unavailable` | `UNAVAILABLE`(location stale) / spot generation stale은 `INVALID_OPERATION` | `unavailable` | `Unavailable` | **3언어 동일** | 스펙 §12 표 "Typed stale error" → `Unavailable`로 명시 |
| J3 | G7 abort 시 seal 해제↔held 제출 순서 | held **폐기**(J9) | 해제→제출 | 해제→제출 | 해제→제출 | **정정** — 52뿐 아니라 28도 제출→해제였다(2차 라운드에서 확인). 20·48과 4언어 구현이 해제→제출이므로 그 순서가 맞음 | relocation 주제에서 52 §5를 링크로 교체할 때 해소. spec-gap G7 → `판정: 문서 모순, session 문서 기준` |
| J4 | G8 abort 주체 명칭 | wire `Coordinator` 필드 | `RelocationRole.SOURCE` | wire `coordinator` | `senderRole` | 코드에 두 이름 모두 없음. wire 필드는 하나 | 문서 용어를 "relocation coordinator(source node가 채우는 wire fence)"로 통일 — session 문서 §8.2 한 곳 |
| J5 | G4 종료 사유 집합 소유 | connector enum과 1:1 | wire 4종·계기 4종 불일치(→§3 이관) | 1:1 | 1:1 | 3언어 모두 connector §6.3 집합을 그대로 구현 | 소유는 그대로(client 스펙). session 문서는 링크 유지. 대장 `기각` |
| J6 | G5 shared permit 소유 문서 | 해당 없음 | 해당 없음 | 해당 없음 | scheduler에 통합 | 문서 소유 문제 | execution 주제로 이월(이미 이관 stub) |
| J7 | R56 lane 정책 타입 (재량 판정 기준 "의미 없는 조합 표현 불가") | flat enum 2값 | 없음(`ZLinkProcessExecutionLanes` 홀더) | variant 3종 ✔ | flat/boolean | 재량의 판정 기준을 cpp만 만족 | **불일치 — dotnet·jvm·node 구현 수정** — 스펙은 그대로. 구현 캠페인 목록에 |
| J8 | R54 binding 없이 push/close | `InvalidOperation` | `INVALID_OPERATION` | `not_configured` | `InvalidOperation` | cpp만 이탈 | **불일치 — cpp 구현 수정** |
| J9 | R49 relay-ready 전 abort 시 held를 source route로 | **폐기** | 재제출 | 재제출 | 재제출 | dotnet만 이탈 | **불일치 — dotnet 구현 수정** |
| J10 | R18 role·Store 없으면 enablement startup 거부 | ✔ | 전역 role 검사만, node 연결 미확인 | bind 시점 검사로 대체 | ✔ | cpp·jvm 이탈 | **불일치 — cpp·jvm 구현 수정** |
| J11 | node 단독 이탈 묶음 — R29(bind 전 Store 읽기, local instance overload), R31(`OwnerLeaseGeneration` 미비교·admission deadline 없음), R32(lifecycle generation 미비교 → 재시작 owner 오거부), R33(rebind 시 owner reply 전 local route 설치), R36·R42(tombstone/unbind가 callback **전**에 실행), R59(held queue `outboundCapacity=4096` 상한) | ✔ | ✔ | ✔ | ✘ | node만 이탈 | **불일치 — node 구현 수정** 7건 |
| J13 | R16 stream-session §8 "node 경계를 넘는 것은 record 4개뿐" | 24·36·38·51 + 42·43·44 | 42·43·44도 SessionState 매칭으로 전달 | 동일 | 동일 | 4언어 모두 relocation control 42·43·44도 node 경계를 넘음. 옛 19 §8 문장이 application 경로만 세고 있었음 | **스펙 오류 정정** — §8 그림·문장을 "application 경로 record 4개 + relocation 중 control 42·43·44"로 |
| J14 | R35·G3 51 전송 실패 시 bounded retry | 있음 | **없음**(단발 send, CompletionStage 폐기) | 있음 | 있음 | jvm만 이탈 | **불일치 — jvm 구현 수정** — D7의 시한이 정해지면 그 값으로 |
| J12 | R48(b) 다른 relocation의 late 44 | 예외 throw | Warning 후 no-op ✔ | silent | silent | dotnet만 이탈(스펙: Warning 후 무시) | **불일치 — dotnet 구현 수정** — J13과 함께 |

## 2. 결정 필요 (추천안 포함)

| # | 항목 | dotnet | jvm | cpp | node | 문제 | 추천 |
|---|---|---|---|---|---|---|---|
| D1 | R22 "서로 다른 Actor를 Spot global queue로 직렬화하지 않는다" | `SpotWide` User Spot은 Spot 공통 gate로 직렬화 | ✔(Actor 실행 문맥에 직접 제출) | eager coroutine 첫 turn이 session thread에서 시작 | ✔ | 이 문장은 [Actor 모델](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md)의 `SpotWide` 실행 모드(공통 gate)와 **모순**. 옛 20 §3부터 있던 문장 | **스펙 오류로 정정**: "session의 실행 문맥으로 직렬화하지 않는다. Actor 사이의 실행 순서는 Actor 모델의 실행 모드(`PerActor`/`SpotWide`)가 정한다." cpp의 첫 turn inline은 별도 불일치로 기록 |
| D2 | R47 commit 순서 "route 적용 → held 제출 → seal 해제" | 해제 후 제출 | 해제 후 제출 | 해제 후 제출 | 해제 후 제출 | 4언어 모두 seal 해제가 held 제출보다 먼저. 모두 같은 직렬 구간이라 held가 새 message보다 먼저 나가는 관찰 결과는 같음 | **스펙 개선**: 순서 문장을 관찰 보장으로 바꿈 — "route 적용과 seal 해제는 같은 직렬 구간에서 일어나고, held message는 seal 해제 뒤 도착한 message보다 먼저 target route에 제출된다." 코드 변경 없음 |
| D3 | R48(a) late/duplicate 44에 `late_session_route_update` **Warning** | debug 레벨 | **Warning 기록 ✔** | 로그 없음 | 로그 없음 | jvm만 스펙대로 Warning 기록, 나머지 3언어 미기록. 로그 이름·레벨은 24/26(observability)이 소유 | 두 갈래 — (a) 스펙 유지·3언어 Warning 추가(작은 구현, jvm이 선례), (b) 스펙을 "기록한다"로 완화하고 이름은 observability 주제에서 정함. **추천 (a)** — 운영자가 relocation 지연을 볼 유일한 신호 |
| D4 | G6/R14 "같은 session type 중복 등록" = 서로 다른 node 간? | 검사 없음 | 검사 없음 | **전역** 검사 있음 | 검사 없음 | 스펙 미지정 → 발산 | **추천: cpp 채택** — "같은 session type을 둘 이상의 node에 등록하면 startup 실패"로 명시(행이 존재하는 이유가 그것뿐). dotnet·node·jvm 구현 수정 |
| D5 | G1 교체 callback 강제 종료 deadline | `DefaultRequestTimeout` 30 s | 5 s 하드코딩 | 5 s 하드코딩 | 30 s, override 가능 | 스펙 미지정 → 2:2 발산(dotnet·node 30 s 설정 가능 / cpp·jvm 5 s 하드코딩) | **추천**: server option `SessionReplacementCallbackTimeout`으로 명시하되 기본값은 사용자 선택 — 30,000 ms(request timeout과 동일 감각) 또는 5,000 ms(중복 연결 안내 callback은 짧아야 함). 나머지 두 언어 구현 수정 |
| D6 | G2(a) Actor factory 없음의 `ErrorKind` | `InternalFailure`/`Rejected` | `NOT_CONFIGURED` | `NotFound` | configuration exception | 4언어 4가지 | **추천 `NotFound`** — 32 §2 정의("handler… 존재하지 않는다")에 가장 가깝고 cpp 기존. 나머지 3언어 구현 수정 |
| D7 | G3 51 전송 retry 상한 | 10 ms 고정, 30 s | retry 없음(J14) | 4회, 10/20/40 ms, queue 1024 | 25 ms→2배, cap 1 s, 30 s | 발산. 관찰 결과: 통지가 결국 도달하는지·언제 포기하는지 | **추천**: 스펙은 "포기 시한 30,000 ms"만 명시(dotnet·node), 간격·횟수는 **언어별 재량**(관찰 결과 같음: 시한 안 도달 또는 포기). cpp의 4회·~70 ms는 시한과 어긋나므로 구현 수정 |
| D8 | R2 admission 실패 시 "새 packet을 읽지 않는다"의 범위 | node 전체 receive 중단 | node 전체 | 해당 peer만 중단, 다른 peer는 계속 | node 전체 중단 | 스펙 미지정(연결별인가 node별인가). cpp만 연결별, head-of-line 회피 | 두 갈래 — (a) node 전체(3/4, Core receive pipe HWM이 socket 단위라 자연스러움), (b) 연결별(cpp, 성능상 우월). **추천 (a)로 명시**하되 (b)를 허용하려면 관찰 결과가 달라지므로 재량이 아니라 계약 변경 — 사용자 판단 |
| D9 | R40 bind 시 target에 Actor 없고 Message Follow route 있으면 relay | 부분 확인 | 거부 사유 미구분(단일 실패 코드), Follow relay 없음 | 미발견 | **미구현**(즉시 `Unavailable`) + ObjectGeneration 다름이 `Unavailable`(스펙 `InvalidOperation`) | 3~4언어에서 bind-시 Message Follow relay 흔적 없음. jvm은 거부 사유도 구분하지 않음 | **추천**: 불일치로 등록하되 relocation 주제(Message Follow 소유) 검토 때 함께 사실 확인 후 확정 |
| D10 | R11 초과 시 "`EMSGSIZE`를 기록" | 리터럴 없음(`"STREAM frame exceeds MaxMessageSize."`) | ✔ | ✔ | ✔ `code='EMSGSIZE'` | 기록 문자열이 관찰 대상인가 | **추천**: 스펙 유지(errno 이름은 Core 계약), dotnet 구현 수정(로그에 `EMSGSIZE` 포함) |

## 3. 이 주제 밖으로 이관

| 항목 | 이관 대상 |
|---|---|
| cpp R7 — HTTP client host별 codec registry 구조 없음 | http-client 스펙 gap |
| node R30 — Message Follow hop 상한 8·duration 30,000 ms(코드 선택) | location-relocation 주제 (`MessageFollowDuration` 소유) |
| node R21 — session sequence가 session별이 아니라 node 단일 카운터 | 관찰 결과 같음(session 안 단조 증가 유지). 기록만 |
| cpp R52 — infrastructure task 개념 이름 없음, `commit_terminal` 동기 실행 | execution 주제(42 progress-isolation) |
| jvm R9·G4 — 계기 label `server_drain` vs 스펙 `server_shutdown`, idle/heartbeat 종료가 `zlink.stream.connections.closed`에 미계상 | observability 주제(25 runtime-metrics) |
| cpp TEST — layout contract test가 옛 20 경로를 엶 | 이동 단계에서 경로 갱신(매핑표 §6) |

## 4. 다음 단계

이 표의 모든 행은 spec gap이다. 처리 순서는 [대장 머리말](../../spec-gap.ko.md)이 정한 대로다 —
**문서 재구성을 먼저 끝내고, gap은 모아서 처리한다.**

1. **지금(문서 수정 단계)**: 자동 판정 중 문서 정정에 해당하는 J1·J2·J4·J13만 session 문서에
   반영한다. 재구성이 만든 오류가 아니라 옛 문서의 오류를 옮기지 않기 위한 것이다.
2. **재구성 완료 후(gap 처리 단계)**: `결정` 열이 `대기`인 행(D1~D10)을 추천안과 함께 한 번에
   결정하고, 스펙 지정 커밋으로 반영한다.
3. **구현 캠페인**: 해소 수단이 구현 수정인 행(불일치)을 작업 목록으로 삼는다. 우선순위는 그
   단계에서 정한다 — dotnet의 abort 시 held 폐기(J9), node의 재시작 owner bind 오거부(R32)는
   관찰 결과가 장애로 이어지므로 목록 상단 후보다.
