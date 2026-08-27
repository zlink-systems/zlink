# ledger — 03-client-server-channel · 04-network-listener-identity

재작성 대상: `09-client-server-channel.ko.md` → `03-client-server-channel.ko.md`,
`10-network-listener-identity.ko.md` → `04-network-listener-identity.ko.md`.
행 위치는 [mapping.ko.md §5.3](mapping.ko.md#53-09-client-server-channel)·
[§5.4](mapping.ko.md#54-10-network-listener-identity)의 R#를 그대로 따른다.

## R# → 새 위치

### 09-client-server-channel

| R# | 새 위치 | 비고 |
|---|---|---|
| R70 | `03-client-server-channel` §1 | Role 표 |
| R71 | `03-client-server-channel` §1 | 미제공 기능 4개, Server 알림은 별도 RouteMesh 필요 |
| R72 | `03-client-server-channel` §2 | Registration key, `NotConfigured`/`NotFound` 구분 |
| R73 | `03-client-server-channel` §2 | startup configuration error 3가지 |
| R74 | `03-client-server-channel` §2 | 여러 process Server 등록, 식별 정보 불일치는 protocol 오류 |
| R75 | `03-client-server-channel` §3 | 발견 방식 표(manual/automatic), 연결 후보 하나로 합침 |
| R76 | `03-client-server-channel` §3.1 | Client만 connection 시작, automatic discovery별 intent |
| R77 | `03-client-server-channel` §3.2 | Server descriptor 필드 5개+owner lease, ready 재확인 조건 |
| R78 | `03-client-server-channel` §3.3 | Location Store 필요 시점, manual도 실제 연결에서 재확인 |
| R79 | `03-client-server-channel` §4 | weight 범위 `0..10000`/기본 `100`, 상대 비중 의미 |
| R80 | `03-client-server-channel` §4 | ready+non-draining 비교, overflow-safe 합산, weight 0/drain 구분 |
| R81 | `03-client-server-channel` §4.1 | local Server 후보 포함, handler 직접 호출 안 함 |
| R82 | `03-client-server-channel` §4.1 | target 선택+submit 단일 작업, 자동 재전송 안 함 |
| R83 | `03-client-server-channel` §4.2 | descriptor revision 증가, local weight는 ChannelName으로 지정 |
| R84 | `03-client-server-channel` §5 | Send/Request 정의, 완료 결과 5종 |
| R85 | `03-client-server-channel` §5.1 | Reply token 재사용 금지, 실패 3가지의 error reply/드롭 구분 |
| R86 | `03-client-server-channel` §5.2 | Downstream request 별도 연결 정보, 원래 request 완료 규칙 |
| R87 | `03-client-server-channel` §6 | Drain 4단계, manual client 통지, 유한한 rejected 결과 |
| R88 | `03-client-server-channel` §7 | 재시작 시 새 generation, client 교체 4단계 |
| R89 | `03-client-server-channel` §7 | reply correlation 비교 규칙, 늦은 reply 폐기 |
| R90 | `03-client-server-channel` §8 | Location Store 장애 시 유지·중단 규칙, fencing deadline |

### 10-network-listener-identity

| R# | 새 위치 | 비고 |
|---|---|---|
| R91 | `04-network-listener-identity` §1 | Bind/Advertised 주소 2종, 공유 network 값, HTTP listener 제외 |
| R92 | `04-network-listener-identity` §2 | listener별 override는 그 listener에만 적용 |
| R93 | `04-network-listener-identity` §2.1 | 기본 BindHost `127.0.0.1`, AdvertiseHost 생략 시 규칙 |
| R94 | `04-network-listener-identity` §2.2 | wildcard 허용 위치, BindHost wildcard면 AdvertiseHost 필수 |
| R95 | `04-network-listener-identity` §3 | Port 0 처리, bind/advertised endpoint 계산식 |
| R96 | `04-network-listener-identity` §3 | manual mode 명시 필요, wildcard·port 0 잔존 시 설정 오류 |
| R97 | `04-network-listener-identity` §3.1 | publisher listener 상태 조회 결과와 제약 |
| R98 | `04-network-listener-identity` §3.1 | 공통 listener 상태 조회, 4종 listener 종류 |
| R99 | `04-network-listener-identity` §4 | listener 종류별 기록 위치 4행 표 |
| R100 | `04-network-listener-identity` §4 | automatic fanout publisher descriptor 게시, manual 미게시 |
| R101 | `04-network-listener-identity` §5 | 재시작 시 새 endpoint+새 generation을 같은 revision에 기록 |
| R102 | `04-network-listener-identity` §5 | listener별 독립 descriptor·generation |
| R103 | `04-network-listener-identity` §6 | Core `NID` 없음, RID는 opaque value, `Node RID`는 역할명 |
| R104 | `04-network-listener-identity` §6 | Core/Framework automatic RID 발급 규칙, transport/logical identity 구분 |
| R105 | `04-network-listener-identity` §6 | RID·identity 발급과 표현 6행 표 |
| R106 | `04-network-listener-identity` §6.1 | diagnostic prefix 문자·길이 제한, Full RID 형식 |
| R107 | `04-network-listener-identity` §6.2 | RID 충돌 시 즉시 startup error, replacement는 새 UUID, Fixed RID 제약 |
| R108 | `04-network-listener-identity` §6.3 | Entry Spot ID 발급 규칙, lifecycle 유지·교체 |
| R109 | `04-network-listener-identity` §6.3 | global Spot ID conflict 처리, descriptor mapping 사용 |
| R110 | `04-network-listener-identity` §6.3 | 예약 형식 caller 지정 시 `InvalidOperation` 거부 |
| R111 | `04-network-listener-identity` §7 | Kubernetes AdvertiseHost, Pod별 개별 발견·연결 |

배치되지 못한 R#: 없음(R70~R111 전부 배치).

## 이동 후 갱신할 링크

두 문서 모두 `spec/server/`에서 `spec/server/02-channel-transport/`로 한 단계 더
들어가므로 상대 경로 깊이가 하나씩 늘었다. 이번 재작성에서는 산출물 파일 2개에만
새 경로를 반영했고, 이 표는 다른 파일이 옛 경로로 이 두 문서를 가리키는 자리를
캠페인 마지막 이동 단계(README §5)에서 함께 갱신하도록 남긴다.

| 위치(옛 문서) | 옛 경로 | 새 경로 |
|---|---|---|
| 09 nav, 10 nav | `README.ko.md` (spec 목차) | 두 갈래 — 주제 목차 `02-channel-transport/README.ko.md`(아직 없음)와 spec 목차 `../README.ko.md`. 이번 산출물에는 두 링크 모두 반영함 |
| 09 nav | `08-channel-messaging.ko.md` | `02-channel-messaging.ko.md`(아직 없음, 이 주제의 다음 재작성 대상) |
| 09 nav | `10-network-listener-identity.ko.md` | `04-network-listener-identity.ko.md`(이번 산출물) |
| 10 nav | `09-client-server-channel.ko.md` | `03-client-server-channel.ko.md`(이번 산출물) |
| 10 nav | `11-spot-model.ko.md` | `03-spot-actor` 주제로 이동 예정(경로 미확정) — 이번 산출물은 mapping이 지정한 `05-transport-liveness.ko.md`로 "다음" 링크를 바꿈(§3.4 nav 지정) |
| 09, 10 본문 전체 | `01-glossary.ko.md#anchor` | `../01-glossary.ko.md#anchor`(글로서리는 아직 이동하지 않았으므로 한 단계만 추가) |
| 09, 10 본문 | `languages/dotnet/interfaces/03-configuration-topology.ko.md` | `../languages/dotnet/interfaces/03-configuration-topology.ko.md` |
| 09 본문 | `languages/dotnet/interfaces/04-channel-messaging.ko.md` | `../languages/dotnet/interfaces/04-channel-messaging.ko.md` |
| 옛 문서 자체(09, 10) | — | 캠페인 절차(§4.6)대로 옛 `09-client-server-channel.ko.md`·`10-network-listener-identity.ko.md` 맨 위에 "재작성 중" 한 줄을 추가하는 일은 코디네이터가 처리(이번 작업 범위는 산출물 파일 3개로 한정됨) |

코드 주석·cpp contract test 영향은 없음(mapping §1, §6 — 이 7개 문서를 경로로 여는
코드는 `channel-messaging`(08→02)만 가리키며, 09·10은 코드에서 경로로 열리지 않음).

## 소유 재검토

- **`04-network-listener-identity` §6(옛 10 §7~§7.3, RID·Spot ID 발급 정책)** —
  mapping §4 S8이 지적한 대로 이 절은 "network listener identity" 범위를 넘는
  시스템 전체 규칙이다. Core raw socket RID 형식, Framework 진단 prefix, Entry Spot
  ID 발급까지 다루며 listener 자체의 bind/advertise 주소와 직접 관련이 없다. 관련
  규칙: R103(Core `NID` 없음, RID 역할명)·R104(자동 RID 발급 규칙)·R105(발급·표현
  6행 표)·R106(diagnostic prefix 제한)·R107(RID 충돌·lifecycle)·R108~R110(Entry Spot
  ID 발급·충돌·예약 형식 거부). 이번 재작성에서는 topic-map이 10 전체를 이 주제에
  배정했으므로 그대로 `04-network-listener-identity.ko.md` §6에 유지했고, §6 첫
  문단에 범위 설명 문장을 추가했다. **재배치 여부(예: 00-foundation의 글로벌
  식별자 정책으로 이동)는 이번에 결정하지 않는다** — 00-foundation 주제 작업 때
  코디네이터가 판단하도록 남긴다.

## spec-gap 후보

이 두 문서(09·10)를 재작성하며 새로 발견한 spec-gap 후보는 없다. mapping.ko.md
§7의 G-candidate 1(29 §4 수신 독점 방지 상한 3축의 실제 값 미정)은 이 두 문서와
무관하며 `05-transport-liveness` 재작성 때 처리한다.
