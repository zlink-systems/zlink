# spec/server 재구성 — location-relocation 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 다섯 번째 주제 `05-location-relocation`의 작업 계획이다. 양식은
> [04-session 매핑표](../04-session/mapping.ko.md)를 따른다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분 초안](../../topic-map.ko.md) ·
[session 판정표](../04-session/judgment.ko.md)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 | 다른 md의 참조 파일 수 |
|---|---:|---|---:|---:|
| [21-location-runtime](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md) | 1,333 | 계약 — Location Store·Relocation Store 사용 순서, generation 체계, Redis record 상호운용 | 50 | 90 |
| [22-location-store-redis](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md) | 252 | 계약(provider SPI) + 구현 스펙(공식 Redis 구현) | 8 | 41 |
| [23-relocation-store-redis](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md) | 286 | 계약(provider SPI) + 구현 스펙(공식 Redis 구현) | 6 | 26 |
| [28-relocation-flow](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md) | 651 | 계약 + 구현 스펙 — Actor·Spot relocation의 단일 handoff 프로토콜 | 17 | 72 |
| [44-internal-relocation-continuity](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md) | 185 | 구현 스펙(내부 설계, 문서 자신이 "공개 규범 스펙 아님"이라 명시) | 2 | 15 |
| [52-internal-relocation-handoff](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md) | 336 | 구현 스펙(내부 설계, 위와 동일 표기) | 0 | 11 |
| [30-host-relocation-flow](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md) | 1,184 | 계약 — Host `Relocate`/`Shutdown`의 host 단위 조율 | 111 | 92 |
| [31-failure-failover-policy](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/06-failure-failover-policy.ko.md) | 277 | 계약 — 장애 시 재시도·재선택 정책 경계 | 42 | 50 |
| **합계** | **4,504** | | **236** | |

### 코드가 이 문서를 경로로 여는 곳

cpp layout contract test(`test_cpp_framework_layout_contract.cpp`)는 이 8개 문서 중 어느 것도 needle
검색 대상으로 열지 않는다(grep 결과 0건) — session·actor-model 문서와 달리 이 주제는 그 test의 갱신
대상이 아니다.

Redis provider 구현체가 22/23을 golden 계약 원본으로 **파일 경로 주석**에 담는다(40여 곳,
`grep -rln "21-location-runtime\|22-location-store-redis\|23-relocation-store-redis"` 결과). 언어별 위치:

- **node**: `framework-locations-redis/src/{opaque-redis-scripts,opaque-store,relocation-store}.ts`, `framework/src/runtime/locations/location-store-repository.ts`, `test/contract/{location-redis-store,location-runtime}.test.js`
- **cpp**: `framework/src/runtime/locations/{base64.hpp,provider_location_repository.hpp}`, `framework/src/runtime/protocol/relocation_envelope_codec.hpp`, `extensions/framework-locations-redis/include/zlink/locations/redis.hpp`, `tests/Zlink.Framework.UnitTests/{test_cpp_framework_relocation_envelope_golden,test_cpp_framework_locations_redis,test_cpp_framework_store_record_golden}.cpp`
- **dotnet**: `Zlink.Framework.Locations.Redis/{ZLinkRedisLocationStore.Opaque,ZLinkRedisRelocationStore}.cs`, `Zlink.Framework/Runtime/Locations/*.cs`(6개), `tests/.../{StoreRecordGoldenTests,RelocationRuntimeTests,ProviderLocationRepositoryAuthorityTests}.cs`
- **java**: `zlink-framework-locations-redis/.../{ZLinkRedisLocationKeys,ZLinkRedisOpaqueLocationStore,ZLinkRedisStringByteArrayCodec}.java`, `zlink-framework-core/.../locations/*.java`(4개), 관련 test 3개

`framework/testdata/location/redis/*.json` 5개(`actor-location-v2`, `authority-store-v1`,
`client-server-server-descriptor-v1`, `fanout-publisher-descriptor-v1`, `mesh-node-descriptor-v1`) 모두
`"notice"` 필드에 `22-location-store-redis.ko.md`를 "Canonical contract"로 명시 인용한다(golden fixture
`store-record-v1.json`과 함께 §21 §2.4·§22 §7이 정의하는 opaque record 형식의 conformance 기준). 이동 시
notice 문자열의 경로도 함께 갱신해야 한다(§6).

## 2. 독자 질문 — 주제 README가 답할 것

| 질문 | 답이 있어야 할 자리 |
|---|---|
| Framework는 object의 현재 위치를 어떻게 찾는가 | README 개요 + `01-location-runtime` §1 |
| Location Store와 Relocation Store는 각각 무엇을 책임지는가 | `01-location-runtime` §1.2 역할 표 |
| Location Store·Relocation Store를 직접 구현하려면 무엇을 보장해야 하는가 | `02-location-store-redis`, `03-relocation-store-redis` |
| 같은 ID로 다시 만든 object와 owner가 바뀐 object는 어떻게 구분하는가 | `01-location-runtime` §2 |
| Actor·Spot을 다른 node로 옮기는 정상 순서는 무엇인가 | `04-relocation-flow` §4 |
| 이동 중 message는 어디로 가는가, 이동 뒤 완료는 언제인가 | `04-relocation-flow` §5, §6 |
| 실패하면 무엇이 남고, 자동으로 어디까지 계속하는가 | `04-relocation-flow` §9, `06-failure-failover-policy` |
| Actor relocation 중 그 Actor에 연결된 session은 어떻게 되는가 | `04-relocation-flow` §7 (→ session 문서 §8 링크) |
| Host maintenance(계획된 host 전체 이동)는 개별 Actor 이동과 무엇이 다른가 | `05-host-relocation-flow` |
| 제한은 무엇인가(chunk 크기, in-flight budget, 페이지 크기, timeout 값들) | 각 문서 수치 절, `01-location-runtime` §1.2 |
| Store 연결이 끊기거나 응답을 못 받으면 무엇이 멈추는가 | `01-location-runtime` §4, §8, `06-failure-failover-policy` §7 |

## 3. 새 구조

```
spec/server/05-location-relocation/
  README.ko.md                     주제 진입 1장
  01-location-runtime.ko.md        21 재작성
  02-location-store-redis.ko.md    22 재작성
  03-relocation-store-redis.ko.md  23 재작성
  04-relocation-flow.ko.md         28 + 44 + 52 병합 재작성
  05-host-relocation-flow.ko.md    30 재작성 (§8 대폭 축소, 04 링크로 대체)
  06-failure-failover-policy.ko.md 31 재작성
```

`target-readme.ko.md` 초안(162-177행)은 `04. relocation-flow`와 `04. host-relocation-flow`에 번호가
겹쳐 있다(오타) — 이 매핑표는 `04-relocation-flow` / `05-host-relocation-flow` / `06-failure-failover-policy`
순으로 번호를 바로잡아 쓴다. target-readme는 이 주제 완료 시 함께 정정한다(§8).

### 3.1 44 + 52 → 28 병합 판정

**병합한다.** 근거:

- 44(185줄)와 52(336줄) 모두 문서 맨 앞에 "공개 규범 스펙이 아닌 내부 설계 문서"라고 스스로 밝히고,
  공개 계약은 28이 소유한다고 명시한다.
- 두 문서 본문의 약 80~90%가 28 §4~§9·§12의 재서술이다 — 같은 8단계 순서(capture → Restore →
  temporary queue → ingress hold relay → cutover → CAS → backlog 병합 → dispatch 개방)를 44는
  "네 개의 경계"(①~④) 틀로, 52는 상태기계(FSM) 틀로 각각 다시 그린다. 1,000ms cutover 대기,
  3,000ms seal timeout, CAS 4개 선행조건, 6행 CAS 결과표, backlog 3단계 순서, send/request 표가
  세 문서에 사실상 동일한 문장으로 반복된다.
- 44·52에서 28에 없는 순net-new 내용은 많지 않다.
  - 44: Message Follow 기본값 30초·최대 8 hop·순환 시 `Unavailable`·generation 불일치 시
    `InvalidOperation` — 그러나 이 값들은 [21-location-runtime §6.3](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md#73-이전-owner로-도착한-message를-새-owner에게-전달한다)이
    이미 정의한다(867-876행). 44는 이 값을 21의 재서술로 인용했을 뿐 새 값이 아니다 — 21을
    canonical 출처로 링크하면 된다.
  - 44: "이동 하나는 하나의 상태 전이 규칙이 소유하고 component별로 독립 진화하는 state로
    쪼개지 않는다"는 내부 구현 결정 — 28의 공개 계약에는 없던 내용이나, 구현 구조 결정이므로
    04의 "구현 결정" 절에 흡수한다.
  - 52: handoff가 소유하는 값 인벤토리(object identity, source/target fence, relocation identity,
    saved-work reference, relay connection, temporary queue) — 28 §2·§4에 흩어진 내용을 표로
    압축한 것. 04에 표 하나로 흡수.
  - 52 §8의 "추가하면 안 되는 relocation 기법" 11개 중 8개는 28의 부정 규칙을 재진술이지만, 3개는
    28에 없다 — target payload 부분조립 자가복구 금지, exact identity가 아닌 도착 순서로 attribution
    금지, **동일 target queue에 대한 두 개의 Actor Join prewarm-prepare 동시 유지 금지(새 identity
    도착 시 기존 prepare abort, 최신 시도가 항상 승리)**. 세 항목 모두 04의 "하지 않는 것" 절에
    새로 추가한다.
- 병합 규모 추정(2차 조사 에이전트 산출): 44+52의 중복분(약 450~490줄)을 제거하고 순net-new
  내용(30~70줄)만 흡수하면 병합 문서는 **약 680~720줄**로 예상된다 — 지정한 ~900줄 상한을 넘지
  않으므로 **분할하지 않는다**.
- 세션 관련 서술(28 §4.7·§7, 44 일부, 52 §5 전체)은 병합 문서에 남기지 않고 세션 문서
  [`04-session/02-session-actor-binding.ko.md` §8](../../../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md#8-actor-relocation-중-session의-책임)로
  링크한다(§4.9 참고) — 이 정리가 병합 문서 크기를 추가로 줄인다.

### 3.2 `01-location-runtime` 절 구성

| 새 절 | 옛 출처 | 서술 종류 | 비고 |
|---|---|---|---|
| 1. Location 개요 — 두 Store의 책임 | 21 §1, §1.1, §1.2 | 계약 | |
| 2. 역할과 책임 — provider vs Framework | 21 §1.2 책임표, §1.3 등록조건 | 계약 | SPI 원칙 공통 문장은 여기 한 번(§4 S8) |
| 3. 같은 ID의 재생성과 owner 변경을 구분하는 값 | 21 §2 전체 | 계약 | §2.1~§2.4를 실제 소제목으로 유지, §2.4는 record 종류별로 재분할(§4 S4) |
| 4. 실행 중인 node와 제공 기능을 찾는다 | 21 §3 | 계약 | |
| 5. Store 연결이 끊기면 새 작업을 막는다 | 21 §4 | 계약 | |
| 6. 현재 위치 record를 읽고 변경한다 | 21 §5 | 계약 | |
| 7. Actor와 User Spot을 만든다 | 21 §6, §6.1~§6.4 | 계약 | |
| 8. Actor·Spot을 다른 node로 옮길 때 이 Store가 하는 일 | 21 §7 요약 + §7.1~§7.2 (StoreVersion·generation 조건만) | 계약 | 상세 handoff는 `04-relocation-flow` 링크로 축소(§4 S7) |
| 9. Restore·완료 기록과 Store의 관계 | 21 §7.3, §7.5, §7.6 | 계약 | |
| 10. Store 응답을 받지 못했을 때 | 21 §8 | 계약 | |
| 11. Host가 종료될 때 Store record를 정리한다 | 21 §9 | 계약 | |
| 12. 구현 및 contract test 검증 요구 | 21 §10 | 검증 | |

### 3.3 `02-location-store-redis` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 범위와 독자 | 22 §1 | 계약 |
| 2. 공개 SPI의 책임 | 22 §2 | 계약 |
| 3. Key, value, version과 clock | 22 §3 | 계약 |
| 4. Conditional atomic batch | 22 §4 | 계약 |
| 5. 크기를 제한한 snapshot scan | 22 §5 | 계약 |
| 6. Cancellation, 결과 유실과 오류 | 22 §6 | 계약 |
| 7. 등록과 provider instance 수명 | 22 §7 앞부분 | 계약 |
| 8. 공식 Redis provider — counter 발급 | 22 §7 "Framework generation counter" 앞부분(counter 표·범위·clean break) | 구현 스펙 | 번호 없던 소제목을 분리(§4 S5) |
| 9. 공식 Redis provider — 5-record opaque 저장 형식 | 22 §7 "Framework generation counter" 뒷부분(ZSET·cmsgpack) | 구현 스펙 | |
| 10. Contract test | 22 §8 | 검증 |

### 3.4 `03-relocation-store-redis` 절 구성

기존 절 구성(§1 계약 범위 ~ §9 Contract test)을 그대로 유지한다 — 2차 조사에서 다른 두 store
문서 대비 문단 벽·중복이 가장 적어 구조 변경이 필요 없다고 확인됐다. 옮기는 것은 §4 S8~S10이
지시하는 SPI 원칙·Redis helper 미제공·clean break 세 문장을 21/22와의 공유 표현으로 정리하는
것뿐이다.

### 3.5 `04-relocation-flow` 절 구성 (28 + 44 + 52 병합)

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Application에서 보이는 결과 | 28 §1 | 계약 |
| 2. 각 주체의 책임 | 28 §2 | 계약 |
| 3. 무엇을 한 번에 옮기는가 — relocation unit과 handoff가 소유하는 값 | 28 §3 + 52 §2 표 흡수 | 계약 |
| 4. 정상 처리 순서 | 28 §4.1~§4.6 | 계약 + 구현 스펙 |
| 5. Message 순서와 완료 의미 | 28 §5 (+ 52 §3.3 send/request 표 대조 확인) | 계약 |
| 6. Location Store 전환 계약 | 28 §6 (+ 52 §4.2 CAS 선행조건 대조 확인) | 계약 |
| 7. Actor relocation 중 Session — 요약과 링크 | 28 §4.7·§7 축소, 52 §5 흡수 후 세션 문서로 이관 | 계약(링크) |
| 8. Actor와 Spot별 차이 | 28 §8 (+ 52 §9 adapter 표 대조 확인) | 계약 |
| 9. Timeout, failure와 cancellation | 28 §9 (+ 44 §4 실패표 대조 확인) | 계약 |
| 10. Message Follow와 정리 | 28 §10, 값은 21 §6.3 링크로 대체(44의 30초·8 hop 흡수처) | 계약 |
| 11. 구현 결정 — 하지 않는 relocation 기법 | 52 §8 (신규 3항목 포함), 44 §5 단일 상태-전이 소유 결정 | 구현 스펙 |
| 12. 보장하는 것과 보장하지 않는 것 | 28 §11 | 계약 |
| 13. 구현 및 contract test 검증 요구 | 28 §12 + 44 §6 + 52 §10 통합(중복 제거) | 검증 |

### 3.6 `05-host-relocation-flow` 절 구성

| 새 절 | 옛 출처 | 서술 종류 | 비고 |
|---|---|---|---|
| 1. 이 문서가 답하는 질문·장애 처리 범위 | 30 §1, §1.1 | 계약 | |
| 2. Application이 선택하는 operation | 30 §2, §2.1, §2.2 | 계약 | |
| 3. Host state와 완료 결과 | 30 §3 | 계약 | |
| 4. Target을 선택하기 전에 확인하는 조건 | 30 §4 | 계약 | |
| 5. Mode에 맞는 target을 선택한다 | 30 §5, §5.1 | 계약 | |
| 6. Concurrent 호출과 cancellation | 30 §6 | 계약 | |
| 7. Relocation unit과 batch 순서 | 30 §7 (앵커 오류 수정, §4 S11) | 계약 | |
| 8. Interruption budget 목표 | 30 §7.1 | 계약 | |
| 9. Unit 하나를 이전하는 순서 — 04를 따른다 | 30 §8, §8.1 | 계약(링크) | 공통 mechanics는 `04-relocation-flow` 링크로 대체, 이 절은 "host가 unit을 무엇으로 나누고 어떤 순서로 여는가"만 남김(§4 S3) |
| 10. Unit 종류별 차이 — 대상 단위와 callback | 30 §8.3~§8.7(공통부 제거, 차이만) | 계약 | |
| 11. 중간에 실패하면 어느 위치를 유지하는가 | 30 §8.8 | 계약 | |
| 12. 대기 중인 message, timer와 session을 옮긴다 | 30 §9 | 계약(session 부분은 링크) | |
| 13. Relocate 완료와 실패 | 30 §10 | 계약 | |
| 14. Shutdown과 Relocate의 경쟁 | 30 §11 | 계약 | |
| 15. State별 admission | 30 §12 | 계약 | |
| 16. 관측 정보 | 30 §13 | 계약 | |
| 17. 구현 및 contract test 검증 요구 | 30 §14 | 검증 |

### 3.7 `06-failure-failover-policy` 절 구성

기존 절 구성(§1~§10)을 그대로 유지한다 — 2차 조사에서 31이 21/28/30의 메커니즘을 거의
재서술하지 않고 정책 판단 축만 소유하는 가장 깔끔한 위임 사례로 확인됐다. 변경은 §10의
"정본" 한 단어를 "이 문서가 소유하는 최종 규칙"으로 바꾸는 것(§4 S12)과 §5의 실패-시점 표가
가리키는 "relay-ready accepted 경계"의 정의 출처를 `04-relocation-flow`로 명시하는 것(§4 S13)뿐이다.

## 4. 읽으면서 발견한 구조 문제

| # | 문제 | 처리 |
|---|---|---|
| S1 | **Session owner 동작(seal 설치, held message 보관, route 전환, command 42/43/44 처리)이 28·44·52·30 네 문서에 반복 서술됨.** 아래 §4.1 표에 전체 위치를 나열한다 | `04-relocation-flow` §7과 `05-host-relocation-flow` §12는 요약 3~4문장 + [`04-session/02-session-actor-binding.ko.md` §8](../../../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md#8-actor-relocation-중-session의-책임) 링크로 축소. `06-failure-failover-policy` §6은 이미 위임이 짧아 손대지 않음 |
| S2 | 28·44·52 세 문서가 같은 handoff 프로토콜(capture→Restore→temporary queue→ingress hold→cutover→CAS→backlog 병합)을 세 가지 틀(순서 서술/경계 4단계/상태기계)로 각각 재서술 — 본문의 약 40%가 중복 | §3.1의 병합 결정대로 44·52를 28에 흡수, 순net-new 내용만 남김 |
| S3 | 30 §8(434줄)이 unit 종류별(§8.3~§8.6)로 거의 동형인 sequence diagram 4개를 그려 28의 per-object 메커니즘(temporary queue·checksum·cutover·CAS)을 온전히 재서술 — "host operation이 unit을 여는 방법만 추가한다"(30 §8 서문)는 선언과 실제 분량이 어긋남 | §8 공통 순서는 "`04-relocation-flow` §4를 따른다" 한 문장 + 링크로 압축, unit별 절은 "이 unit이 무엇을 하나의 relocation 단위로 묶는가"라는 차이만 남김. diagram은 host 조율(batch 순서)을 보여주는 1개만 유지 |
| S4 | 21 §2.4가 138줄 문단 벽(preimage 표, canonical JSON 필드, MeshNode/ClientServer/fanout/authority record 4개 표가 소제목 없이 연속) | record 종류별 소제목(MeshNode descriptor / ClientServer server descriptor / Fanout publisher descriptor / Authority record)으로 분리 |
| S5 | 22 §7의 "Framework generation counter" 소절이 번호 없는 `###`로 80줄(counter 발급 + clean break 운영절차 + 5-record 저장방식 + cmsgpack 인코딩)을 담아, 이 문서에서 가장 규범적으로 중요한 내용이 목차에 드러나지 않음 | §8 "counter 발급"과 §9 "5-record opaque 저장 형식"으로 분리(§3.3) |
| S6 | 21 §7.1이 65줄 문단 벽 — 식별자 정의(RelocationId 등), 저장 위치 표, target 공간확보 확인표, SpotWide·PerActor 규칙이 소제목 없이 이어짐 | 소제목 4개로 분리 |
| S7 | 21 §1.4(8단계 개요)와 §7·§7.3(같은 8단계의 상세)이 사실상 같은 순서를 두 번 서술 — 1,000ms fallback 값이 §1.4·§7·§7.2·§7.3 네 곳에 반복 | `04-relocation-flow`가 상세 소유, `01-location-runtime`은 §1.4의 개요 하나만 남기고 §7은 "Location Store record·generation·CAS 조건"만 남긴 뒤 `04`를 링크(원 §7 서문이 이미 이 경계를 선언했으나 본문이 지키지 않았음) |
| S8 | 22·23 두 문서에 "SPI type은 provider abstraction package가 소유하고, 외부 provider는 application·Actor·Spot package에 의존하지 않는다"는 거의 동일한 문장이 각각 반복 | 공통 SPI 원칙 한 문장으로 통합하고 서로 링크 |
| S9 | "Location Store·Relocation Store를 하나의 interface나 Redis 전용 등록 함수로 묶지 않는다" 규칙이 21 §1.3·23 §8에 반복 | 21 §1.3이 소유, 23은 링크 |
| S10 | "clean break, 하위호환 경로 없음" 문구가 22 §7·23 §8에 거의 동일하게 반복 | 공통 문장으로 통합 |
| S11 | 30 문서의 `<a id="7-relocation-unit과-실행량-제한">`(365행)가 실제 헤더 텍스트 "7. Relocation unit과 실행 순서"(367행)와 달라 — 이전 제목("실행량 제한")의 잔재 | 재작성 시 앵커를 헤더에서 자동 생성하므로 자연히 해소 |
| S12 | 31 §10이 "이 문서가 공개 장애 동작의 **정본**이다"라고 쓰는데, 다른 문서(28 §8 서문 등)는 같은 개념을 "단일 기준"이라 쓴다 — 희귀 한자어 용어 불일치(가이드 원칙 7.3) | "이 문서가 소유하는 최종 규칙"으로 통일 |
| S13 | "relay-ready accepted 경계"라는 동일 개념을 30(정의)과 31(정책 판단축으로 재사용)이 각각 서술하지만, 정의 자체의 소유 문서가 30인지 28인지 30 안에서도 §1.1/§8.8/§10 세 곳에서 반복 정의됨 | 이 경계의 정의는 `04-relocation-flow`가 한 곳에서 소유(28이 이미 §4.4에서 "비가역 경계"로 정의), `05`·`06`은 정의를 재서술하지 않고 링크만 |
| S14 | 30 §9(대기 중 message·session 이관 표)의 "Actor에 연결된 session" 행이 `04`(28)와 세션 문서가 이미 소유하는 내용을 다시 서술 | `05` §12를 "이 항목은 `04` §7 / session §8이 소유" 링크로 압축 |
| S15 | 28 §12·44 §6·52 §10 세 개의 거의 동일한 contract-test 체크리스트(각 10~30항목)가 병렬로 존재 | 병합 시 하나로 통합, 중복 항목 제거 |
| S16 | 52의 상태기계(stateDiagram-v2) 다이어그램과 28의 sequence diagram이 같은 프로토콜을 다른 표기법으로 두 번 그림 | 병합 문서는 28의 sequence diagram 하나만 유지, 상태기계는 제거(순서·되돌림 규칙은 이미 본문 규칙 문장으로 서술됨) |
| S17 | 44·52 두 문서 모두 "문서 성격 — 공개 규범 스펙이 아닌 내부 설계 문서"라는 동일한 disclaimer 문단을 각자 서두에 반복 | 병합으로 자동 해소(병합 문서 하나에는 이 disclaimer 자체가 필요 없음 — `04`는 계약+구현 스펙을 함께 담는 문서로 두 층 표시만 사용) |

### 4.1 Session owner 서술 중복 — 전체 위치

캠페인 지시(세션 topic §1 J3·spec-gap G7, 세션 문서 §8이 이제 이 책임을 소유)에 따라, 이 주제의
문서들이 세션 owner 행동(seal, held message, route 전환, command 42/43/44)을 재서술하는 곳을
전부 나열한다. 재작성 시 이 목록 전체를 `04-session/02-session-actor-binding.ko.md` §8 링크로
교체한다.

| 문서 | 위치 | 내용 |
|---|---|---|
| 28 | §2 표(38-40행) | Session owner 책임 요약(seal·route 전환·target 미선택) |
| 28 | §4.1(88-90행) | Seal은 source dispatch 정지 전에 설치 |
| 28 | §4.7 전체(281-303행) | Route 적용·seal 해제 시점, 3,000ms timeout, timeout↔update 경쟁, late update Warning, pre-accepted 실패 시 abort 처리 |
| 28 | §7 전체(484-505행) | Session owner가 검증하는 4개 값, Store 재조회 안 함, one-way route update, duplicate no-op |
| 28 | mermaid `S` lane(326-397행) | Seal 요청/응답, route update, seal-timeout 분기 |
| 44 | 91-94행 | Message Follow가 선택 기능이 아닌 이유(Session이 의존) |
| 44 | §4(138-142행) | Bound-Actor route update 전송, no-reply, late/duplicate Warning |
| 52 | §5 전체(197-244행) | Session owner는 relocation coordinator가 아님, 검증 5값, 부정목록 5개, held message 제출 순서, abort 처리 |
| 52 | §5 mermaid(204-223행) | Seal→Restore→cutover→CAS→route-update 흐름(28 mermaid와 동형) |
| 52 | §6 Session owner 행(246-256행) | 검증 경계 표 |
| 52 | §7(258-267행) | 1,000ms/3,000ms 값, first-processed-wins |
| 52 | §10 항목(328-329행) | held-message submit-then-seal, cutover/route-update one-way |
| 30 | 590-593행 | `SessionRelocationSealTimeout` 3,000ms, timeout 시 Session 종료 |
| 30 | mermaid opt block(636-638, 697-700, 773-775, 843행) | route 적용·held 제출·seal 해제 — unit 종류별로 4번 반복 |
| 30 | §9 표(958행) | "Actor에 연결된 session" 행 |
| 30 | §10 표(985행) | 완료 지점 "Session route update 적용" 행 |
| 30 | 961-965행 | Command 44 적용 언급(20-session-actor-dispatch §5로 위임하지만 앞뒤에 동작 자체를 이미 서술) |
| 31 | §6 전체(191-208행) | Session physical connection 유지, binding token 필요, 이전 process로 복원 안 함 |
| 31 | §9 항목(250, 263행) | Session binding 재사용 금지, Session owner 복원 금지 |

### 4.2 G7 처리 확인

[spec-gap G7](../../spec-gap.ko.md)은 52 §5가 relay-ready 전 abort 순서를 "held를 source route로
제출 → 그 seal만 해제"로 적어, 20·48(현 session 문서 §8.1)의 "matching seal 해제 → held를 source
route로 제출" 순서와 반대로 서술한다고 기록한다. 이번 2차 조사(28/44/52 담당 에이전트)가 세
문서와 세션 문서를 대조한 결과, **28(299-301행)과 44(§4 121행 요약)는 이미 세션 문서와 같은 순서
("durable abort와 source queue 복원을 먼저 확정한 뒤 command 44 abort를 one-way로 보낸다")를
쓰고 있고, 52만 반대로 서술한다**는 G7의 진단을 재확인했다. 병합 문서(`04`)는 28·44·session 문서와
같은 순서를 채택하고, 52의 해당 문장은 병합 과정에서 자연히 정정된 문장으로 대체된다 — 이
매핑표가 G7의 실제 정정 지점이다. spec-gap 대장에는 새 행을 추가하지 않고 G7의 "결정" 열
갱신만 이 주제 완료 시 반영한다.


## 5. 규칙 등가성 대장

재작성 뒤 이 표의 모든 행이 새 문서의 정확히 한 절에 있어야 한다. "새 위치" 열은 재작성 뒤
채운다. 옛 문서별로 묶는다. 이 주제는 규모가 크므로 간결함보다 완결성을 우선한다 — 서로 밀접한
하위 사실은 한 행에 세미콜론으로 묶되(04-session 대장의 R14·R21·R34 방식과 동일), 숫자·상태·
오류값은 모두 남긴다.

### 5.1 21-location-runtime

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R1 | Spot 정의(Entry/User/Instance), owner는 동시에 둘 아님, Location Store/Relocation Store 정의, handoff payload는 어느 Store도 안 거침 | 21 §1 (18-37행) | |
| R2 | Framework가 보장하는 6개 결과(현재 service/주소 탐색, owner 하나만 인정, 수용공간 미리확보, 이중생성 금지, 이전owner 뒤늦은 변경 차단, host교체중 state·미실행작업 복원) | 21 §1.1 (47-54행) | |
| R3 | Relocation은 source·target process 실행 중에만 진행; process 종료 후 자동 failover 없음; Store 응답 유실 시 재확인은 failover 아니라 필수 확인 | 21 §1.1 (56-59행) | |
| R4 | Core transport는 byte만 전달, 위치·생성·relocation 상태 해석 안 함 | 21 §1.1 (61-62행) | |
| R5 | 두 Store 책임 분리 표(Location=owner/위치/세대/membership/수용공간/진행상태, Relocation=cold activation 생성정보+최초message, pending reply/완료결과) | 21 §1.2 (68-71행) | |
| R6 | 이동 대상 목록 제한 — 시스템 전체 상한 없음, User Spot 하나의 Actor 총수 무제한, 목록 한 페이지 최대 1,024개·1 MiB | 21 §1.2 (85-89행) | |
| R7 | 목록 항목 하나 = identity·generation·membership·변경정보만(state·payload 없음); 상위목록도 페이지분할, Location Store가 시작위치·전체항목수·내용확인값 3값 보관 | 21 §1.2 (91-112행) | |
| R8 | 복원 검증 = payload(전체길이·chunk수·checksum) + 목록(전체항목수·내용확인값) 두 검증 모두 성공해야 복원 시작; Actor·Spot용 Store 분리 안 함 | 21 §1.2 (114-117행) | |
| R9 | 외부 provider는 Location·Relocation Store interface만 구현, Framework 규칙 직접 구현 안 함; 책임분리표(provider=의미없는 key/bytes 읽기+조건부쓰기 / payload 저장·읽기·삭제, Framework=해석+수용공간계산+실패정리) | 21 §1.2 (119-127행) | |
| R10 | MeshNode·owner lease·ClientServer·fanout descriptor와 authority record의 Redis key·byte표현은 22/23이 정하는 공개계약; provider 보조색인은 공개계약 아님; Redis provider는 계약 밖에서 Lua script 등 사용 가능하나 Framework가 직접 호출 안 함(다른 DB provider도 구현 가능) | 21 §1.2 (133-147행) | |
| R11 | Application은 위치조회·readiness API만 쓰고 Store provider 직접 호출 안 함 | 21 §1.2 (149-150행) | |
| R12 | Location·Relocation Store는 각각 정확히 한 번 등록, 하나의 interface나 Redis 전용 함수로 묶지 않음 | 21 §1.3 (154-155행) | |
| R13 | Object role Client·Server인 MeshNode는 Location Store 필수(없으면 socket 열기 전 startup error, process 내부 대체 Store 없음); role None은 create·find·message·factory 미제공 | 21 §1.3 (168-171행) | |
| R14 | relocation policy(RecreateOnRelocation=state 없이 새 instance, PreserveStateWith=state 복원); 이 policy 하나라도 등록했거나 Instance Spot factory 등록한 설정엔 Relocation Store 정확히 하나 필수(누락·중복 시 startup error); Instance Spot 없고 모든 factory가 DisableRelocation이면 불필요 | 21 §1.3 (174-183행) | |
| R15 | 등록 성공 시 Framework가 dispose 책임, 정확히 한 번 dispose; 같은 connection 공유 시 종료시점은 provider 결정 | 21 §1.3 (185-187행) | |
| R16 | 정상 relocation 8단계(확인 → target 공간확보 → turn 종료 후 capture+Captured 기록 → Restore 요청(전체길이·chunk수·checksum)+chunk 전송 → target temporary queue 등록+조립+checksum 확인+복원+relay 준비통지+ingress hold relay+cutover → target이 cutover 수신 또는 relay준비reply 후 1,000ms 경과 시 CAS(owner·membership·수용공간) → 저장작업·relay·temporary작업 순서대로 큐+regular route 전환+dispatch는 닫아둔 채 lifecycle 완료 후 처리 시작 → source는 완료reply 안 기다리고 Message Follow 유지, payload는 cutover submit 완료 후 정리) | 21 §1.4 (201-224행, mermaid 226행) | |
| R17 | Handoff payload와 owner 변경은 2PC로 안 묶음; RelocationId+target attempt+source fence로 결합, owner CAS 한 번으로 새 owner 공개 | 21 §1.4 (256-259행) | |
| R18 | Relocation Store 사용여부 표 7행(Same-node join=payload 안 만듦, DisableRelocation cross-node=capture 전 거부, RecreateOnRelocation·PreserveStateWith cross-node=Relocation Store 미저장, Host maintenance=완료기록만 저장, Cross-node JoinSpot·JoinEntrySpot=직접전송, Instance Spot 최초생성=Relocation Store 저장(DisableRelocation이어도)) | 21 §1.4 (264-272행) | |
| R19 | (OwnerId, LeaseGeneration) 조합, process 재시작 시 다른 조합 발급; OwnerId=재사용불가값, LeaseGeneration=Store counter 조건부 증가 발급(0 미사용, 이전 process 늦은 요청 거부용, owner 변경횟수와 무관) | 21 §2.1 (284-290행) | |
| R20 | Host 유효성=owner lease 만료시각 기준; Framework는 필요한 OwnerId record 직접 읽음(전체 열거로 판단 안 함); 새 owner 자격 획득·만료자격 인계 시에만 새 LeaseGeneration 발급, 연장·정상해제는 값 불변; counter가 2^63-1이면 GenerationExhausted(재시도해도 불변) | 21 §2.1 (292-303행) | |
| R21 | ObjectGeneration(삭제후 재생성 구분, relocation 시 유지)·AuthorityOwnerGeneration(owner 변경순서)·StoreVersion(provider 발급 CAS용)·OwnerLeaseGeneration(owner process 실행표시); Framework가 object·owner generation을 counter로 발급, StoreVersion만 provider 발급 | 21 §2.2 (310-322행) | |
| R22 | 저장 generation 범위 1..2^63-1, 2^63-1은 소진 sentinel(저장만, 미발급), 발급범위 1..2^63-2; sentinel 저장 시 다음 발급 GenerationExhausted(Store 불변, authority를 오류상태 기록, network command 안 보냄); counter를 0으로 되돌리거나 재사용 안 함 | 21 §2.2 (325-329행) | |
| R23 | Authority key=전역 ActorId·SpotId, UTF-8 1..255 bytes, 대소문자까지 동일해야 동일 ID, normalization 안 함; Spot 종류 Entry\|User\|Instance; MeshName은 identity key 일부 아님 | 21 §2.3 (341-344행) | |
| R24 | 저장항목표(변경충돌방지값: StoreVersion·ObjectGeneration·AuthorityOwnerGeneration·OwnerId·OwnerLeaseGeneration, 배치정보: Reserved\|Active·종류·type·target세대·수용공간, StoreNow, Framework 내부데이터); Key·배치정보는 내부데이터에 재저장 안 함 | 21 §2.3 (349-357행) | |
| R25 | 필요수용공간표(Actor=slot 1, User·Instance Spot=Spot slot 1+종류·type slot 1, User Spot+member 이동=Spot slot 1+type slot 1+member수만큼 Actor slot); slot 범위 0..2^31-1, 한 번에 확보하는 묶음엔 1개 이상 양수 slot 필요 | 21 §2.3 (359-366행) | |
| R26 | Authority record엔 자동만료 없음(owner lease 종료 뒤도 유지); 복구작업만 StoreVersion 조건으로 owner 교체·삭제; record 없으면 StoreNow만 반환, 임시 StoreVersion·generation 안 만듦 | 21 §2.3 (369-373행) | |
| R27 | 5개 record(MeshNode·owner lease·ClientServer·fanout·authority)는 opaque record로 저장; logical key preimage의 SHA-256(소문자 16진수)을 Redis key 마지막 segment로 사용 | 21 §2.4 (377-383행) | |
| R28 | Preimage 5종 형식(`mesh-node\0{MeshName}\0{hex(RoutingId)}` / `owner-lease\0{OwnerId}` / `client-server\0{ChannelName}\0{hex(RoutingId)}` / `fanout-publisher\0{ChannelName}\0{hex(RoutingId)}` / `authority\0{actor\|spot}\0{Id}`); hex(RoutingId)는 raw bytes 소문자16진수, 나머지는 UTF-8 그대로(길이접두사 없음), `\0` byte는 값에 불허 | 21 §2.4 (385-397행) | |
| R29 | Authority preimage 2번째 segment는 literal `actor`·`spot`(같은 문자열 ID라도 actor·spot은 다른 key); Spot 종류는 segment 공유(한 Id당 authority row 1개); golden fixture `store-record-v1.json` | 21 §2.4 (398-403행) | |
| R30 | Value는 provider 미해석 canonical JSON, 최소 field(recordVersion=1(미인식 시 명시적 실패)·ownerId·leaseGeneration(authority는 ownerId·ownerLeaseGeneration)·descriptorRevision(owner lease·authority엔 없음)·descriptor(동일)) | 21 §2.4 (405-414행) | |
| R31 | generation·revision류 정수 field는 JSON string(number 아님); weight·limit·capacity는 JSON number; RoutingId는 소문자16진수 문자열; timestamp는 Unix epoch ms 문자열; 세 record 모두 descriptor 안에 자신의 ownerId·leaseGeneration·descriptorRevision 재기록 | 21 §2.4 (415-425행) | |
| R32 | 언어별 provider의 내부 storage-row 버전 counter는 opaque record의 cmsgpack `version` member가 이미 담당하므로 canonical JSON에 재기재 안 함 | 21 §2.4 (425-429행) | |
| R33 | MeshNode descriptor 필드 14개(meshName·routingIdHex·lifecycleGeneration·descriptorRevision·endpoint·entrySpotId·channelWeights·applicationVersion·objectCapabilities·objectRole·placementWeight(기본100)·capacity·activationConcurrency·maintenanceWave·state·securityIdentity·ownerId·leaseGeneration·updatedAtEpochMs) | 21 §2.4 (431-454행) | |
| R34 | ClientServer server descriptor 필드(channelName·serverRoutingIdHex·lifecycleGeneration·descriptorRevision·endpoint·weight(0..10000,기본100)·state·securityIdentity·ownerId·leaseGeneration·updatedAtEpochMs) | 21 §2.4 (455-469행) | |
| R35 | Fanout publisher descriptor 필드 = ClientServer와 동일하되 weight 없음 | 21 §2.4 (471-485행) | |
| R36 | Authority record는 4언어 모두 하나의 logical key opaque-record 행; objectGeneration은 Store 전역 단조 sequence 발급(identity별 단조성도 보장), counter key·발급계약은 22 §7 | 21 §2.4 (487-490행) | |
| R37 | Authority canonical JSON 최소 field(recordVersion=1·payload(base64)·objectGeneration·authorityOwnerGeneration·ownerId·ownerLeaseGeneration·allocation(state·objectKind·stableType·descriptor·descriptorLifecycleGeneration·capacity)·pendingCreation); payload 제외 정수 field는 JSON string | 21 §2.4 (492-504행) | |
| R38 | Relocation Store payload는 이 opaque record 미사용, 별도 버전 매긴 key공간·형식은 23 소유; 각 언어는 golden fixture conformance test 실행 필수 | 21 §2.4 (506-512행) | |
| R39 | descriptor=host가 게시하는 network주소·실행상태·제공기능; automatic discovery=Framework가 Store에서 읽어 connection 자동구성, owner lease 유효성 직접 확인 | 21 §3 (516-522행) | |
| R40 | Descriptor 계약표(Revision=0아닌 증가값, provider 전체 공유 아님, 2^63-1 초과 시 host를 Error로·게시중단 / Page=항목1..1000개, 최대4 MiB / Descriptor 하나=최대1 MiB, 등록type·state adapter 지원목록 각 최대1,024개) | 21 §3 (524-528행) | |
| R41 | Framework는 목록 읽기 전후 변경번호 확인, 같을 때만 전체 페이지 사용; provider는 전체 목록을 Lua memory에 한꺼번에 만들거나 SCAN cursor를 Framework에 노출 안 함 | 21 §3 (530-532행) | |
| R42 | 같은 Revision `RENEW`는 무해한 no-op(ignored·stale, 재저장 안 함); 내용변경 시 Revision 반드시 증가, 반복돼도 error 아니고 덮어쓰지도 않음 | 21 §3 (534-537행) | |
| R43 | Host는 startup 중 descriptor 전체를 먼저 생성; 크기초과 시 일부를 자르거나 나눠 게시 안 하고 startup 전체 실패; application state의 format·version은 descriptor에 안 넣음 | 21 §3 (539-541행) | |
| R44 | Descriptor 존재만으로 message 전송 가능한 것 아님; RouteMesh·ClientServer는 handshake+사용승인 완료해야 ready; Fanout subscriber는 첫 정상 record 또는 liveness beacon 수신 후 ready(beacon은 application message 아님) | 21 §3 (543-549행) | |
| R45 | Weight 표(Weight=0..10000, 기본100, 0이면 배치·relocation 대상 제외 / Actor·Spot 전체 limit=기본0=무제한, 양수1..2^31-1 / type limit=동일 / 음수 limit=startup configuration error) | 21 §3 (558-563행) | |
| R46 | Entry Spot은 Spot수 미포함(Actor는 전체사용량 포함), Actor type별 limit 없음; Location Store 기록이 최종기준, descriptor의 count는 운영자용 복사본 | 21 §3 (565-568행) | |
| R47 | 배치·Entry Spot 전송 시 target descriptor+host 실행세대+Entry Spot ID를 함께 고정(SpotId 문자열 분석으로 계산 안 함) | 21 §3 (570-571행) | |
| R48 | Framework는 owner lease·Serving상태·남은수용공간 확인 후 weight 비율로 target 선택; Weight 0이 돼도 이미 Ready인 object·완료된 reservation 취소 안 함 | 21 §3 (573-576행) | |
| R49 | local admission deadline=Store 응답시각 기준 "새작업받을수있는마지막시각"; startup 검증관계 `renew interval + renew timeout < owner lease TTL - owner lease fencing margin` | 21 §4 (580-590행) | |
| R50 | 기본값표(Renew interval 5초·Owner lease TTL 15초·Renew timeout 3초·Owner lease fencing margin 5초); 모두 양수, 위반 시 startup error | 21 §4 (592-599행) | |
| R51 | StoreNow·ExpiresAt으로 남은시간 계산, local monotonic 시각도 함께 사용; owner lease 갱신 한 번이 host 전체 local admission deadline 갱신, object별 deadline은 이 시각 연장 불가 | 21 §4 (602-605행) | |
| R52 | Deadline 초과·실행조합 불일치 시 차단대상 4항목(descriptor 게시·automatic RID owner변경, Actor·Spot·Instance message·timer callback 시작, factory·restore 결과확정, relocation source·target 상태변경·수용공간확보); 이미받은작업의 결과처리·정리는 별도 deadline 내 진행가능하나 만료된 owner 자격으로 새 Store 변경 안 만듦 | 21 §4 (607-618행) | |
| R53 | Reserve·Preserve·NewOwner·Commit·Abort는 Framework 내부작업이름(Store public method 아님); Read는 Missing(StoreNow)·Found(record,StoreNow) 반환, 기존record 변경 시 StoreVersion CAS 확인 | 21 §5.1 (622-631행) | |
| R54 | 작업표(Reserve=Missing→Reserved,ObjectGeneration·첫AuthorityOwnerGeneration·수용공간발급 / Commit=Reserved→Active / Abort=Reserved→Missing / Preserve=Active owner·generation·수용공간유지,StoreVersion+내부데이터만변경,target정보없어야함 / NewOwner=Active를target owner로,ObjectGeneration유지+AuthorityOwnerGeneration증가,확보된target수용공간사용 / Delete=Active+색인제거,수용공간같은요청에서감소) | 21 §5.1 (633-640행) | |
| R55 | Reserved에 Preserve·NewOwner·Delete 적용 시 Conflict(불변); Active owner변경은 NewOwner 또는 User Spot 전체이동 최종변경으로만; Preserve·Delete는 현재 owner lease 검증, NewOwner는 target lease+확보공간 검증(record 없거나 lease 오래되면 Conflict, target정보 조합자체 오류면 Store 호출 전 Framework 내부오류) | 21 §5.1 (642-650행) | |
| R56 | 일반 Preserve엔 relocation reservation 정보 없음; standalone relocation에서만 완료기록 payload위치 갱신·target 준비완료 기록 시 reservation정보 전달가능; 성공 시 reservation 기대 StoreVersion도 같은요청에서 갱신 | 21 §5.1 (652-657행) | |
| R57 | 복구작업의 여러페이지 읽기는 첫페이지 시작시점 목록을 끝까지 유지; 한페이지 1..1000개, 최대4 MiB; 첫요청엔 다음페이지 값 없음, 이후값은 최대4,096 bytes(내용해석 안 함), 만료·다른목록읽기용 값이면 ScanExpired | 21 §5.2 (661-667행) | |
| R58 | Provider는 key byte순서로 반환; 페이지에서 찾은 record는 변경후보일 뿐, Framework가 재읽고 처음 version이 그대로일 때만 변경; 같은 시점 목록·삭제표시 정리방법은 공개계약 아님 | 21 §5.2 (669-672행) | |
| R59 | Actor·User Spot은 Create·GetOrCreate로 생성(Instance Spot은 별도 API 없음, 최초message 받은 node가 생성); ID결정(Actor=caller지정, User Spot Create=Framework가 UUID v4 발급, GetOrCreate=caller지정 SpotId+type, Entry Spot=Framework만 발급) | 21 §6 (676-685행) | |
| R60 | InMesh 지정 시 해당 Mesh 사용, 생략 시 role Mesh 1개면 사용(후보없으면 NotConfigured, 2개이상이면 InvalidOperation, 지정Mesh 없으면 NotFound); Create call은 한번만 제출(하나의 deadline), 중복지정·재제출 시 InvalidOperation; 생성요청 저장크기 최대1 MiB, Location Store에만 저장 | 21 §6 (687-696행) | |
| R61 | Target 조건(Serving+stable type등록+owner lease유효+수용공간유효, weight>0 중 선택); Reserve는 record 없을 때만 Creating+수용공간 기록(동시 ObjectGeneration·AuthorityOwnerGeneration 발급); 실패시 해당 host세대 제외하고 deadline까지 다른후보 선택가능, 이미 만든 결과 있으면 그 결과를 따름 | 21 §6 (714-723행) | |
| R62 | Remote User Spot생성=command 47, Actor생성=command 49; source·target 실행세대·ID·type·처음확보record·StoreVersion 모두 확인 | 21 §6 (725-727행) | |
| R63 | Callback 결과표(승인=Creating→Ready+공간사용중+Created기록 / Application거절=Creating삭제+공간반환+Rejected기록 / Exception=Creating삭제+공간반환+typed Failed기록 / Callback전 Framework실패=같은record취소+공간반환, 최종결과안만듦) | 21 §6 (729-734행) | |
| R64 | 현재record별 Create·GetOrCreate 결과표(같은type Ready→AlreadyExists / 같은type Creating→AlreadyExists+진행중결과대기 / 다른Actor type→TypeMismatch / 다른Spot종류·type→TypeMismatch); 대기중 deadline초과 시 DeadlineExceeded | 21 §6 (741-750행) | |
| R65 | 응답유실 시 source Node RID+host세대+128-bit OperationId로 재확인; 저장 최종결과 계약(형식 `creation-operation-terminal-v1`+SHA-256, 최대1,048,576 bytes, 최초deadline+5분 보관, 같은요청만 재응답가능) | 21 §6 (752-761행) | |
| R66 | Ready변경+최종결과기록은 한 번에 처리; 취소·timeout·response loss만으로 생성실패 판단 안 함, 다른owner에 자동재제출 안 함; Remote생성 완료는 command 20의 Existing\|Created\|Rejected | 21 §6 (763-767행) | |
| R67 | Instance Spot cold activation: Spot요청표시+Spot없을때만 target이 생성; 현재위치·option별 동작표(Ready record있음=저장정보사용 / record없고InMesh지정=해당Mesh선택 / record없고InMesh생략=0개NotConfigured·2개이상InvalidOperation / Type생략=1개선택·0개NotFound·2개이상InvalidOperation) | 21 §6.1 (771-781행) | |
| R68 | 생성정보 포함값(type·Mesh·target descriptor·SpotId·source Node RID+host세대·선택적source SpotId·operation ID·reply correlation·deadline·command 39정보·최초message); Source는 owner·generation 미리 안 만듦 | 21 §6.1 (806-809행) | |
| R69 | Target은 Location Store+process내부목록 함께확인; record없을때만 최초message를 Relocation Store 저장+Creating+수용공간확보; 동시 여러target 시도해도 성공한 하나만 factory 실행 | 21 §6.1 (811-814행) | |
| R70 | Ready기록후에도 최초message 실행완료기록까지 저장데이터 유지(queue삽입만으로 삭제안함); handler완료기록후 Location Store 사용종료기록+payload삭제; source는 최초message 재전송 안 함 | 21 §6.1 (816-820행) | |
| R71 | 이 복구정보는 Ready Instance Spot 최초생성 시에만 사용(Actor·다른Spot종류·Creating·Closing·Relocating·host relocation엔 사용 안 함) | 21 §6.1 (822-824행) | |
| R72 | Process종료 시점별 처리표(payload저장뒤Creating전=보관기한종료시삭제 / Creating뒤Ready전=같은record·generation으로 계속 또는 취소 / Ready뒤최초message복원전=저장데이터로복원, 그전엔 새message 안받음) | 21 §6.1 (826-830행) | |
| R73 | 이미 Ready면 원래요청을 현재owner에 한번 전달; Creating이면 같은 생성결과 대기; 이전generation instance에선 message 실행 안 함; User Spot이거나 type 다르면 TypeMismatch | 21 §6.1 (832-835행) | |
| R74 | Find(global ID)는 현재 Ready object만 반환(새object 안 만듦); ActorRef·SpotRef=전역ID+ObjectGeneration(0아닌 63-bit unsigned, JSON은 decimal string)+MeshName+NodeRid(조회당시 위치, target·배치조건으로 안 씀) | 21 §6.2 (839-843행) | |
| R75 | Bound session accessor의 ActorRef snapshot은 route switch 뒤 target MeshName·NodeRid로 갱신되지만 binding route 자체는 Location Store가 저장·선택 안 함 | 21 §6.2 (843-845행) | |
| R76 | Destroy·Close는 caller가 넘긴 ref의 generation만 대상(없으면 false, 다른generation 있으면 stale-generation error, 이동중이면 typed moving error, 최신ref 임의탐색 안 함); Remote종료=command 48 userSpotClose, command 20이 closed결과 1회 반환 | 21 §6.2 (847-856행) | |
| R77 | Ready위치 cache 가능값(ID·ObjectGeneration·AuthorityOwnerGeneration·StoreVersion·owner lease·node세대·route); RouteCacheMaxAge 기본15초(owner 새작업 마지막시각 못넘음); Missing·Creating·Store오류는 cache 안 함; 더 높은 StoreVersion이나 lease만료 확인 시 즉시 제거 | 21 §6.3 (860-865행) | |
| R78 | MessageFollowDuration 기본30초, 0이면 cache·전달 둘다 끔; 둘다 사용 시 cache 보관시간이 전달기간보다 최소5초 짧아야 함(위반 시 configuration error) | 21 §6.3 (867-870행) | |
| R79 | 이전owner는 이동완료 시 기록한 source→target 정보만 사용(Store 새로 안 읽음); 새owner의 AuthorityOwnerGeneration은 이전값보다 커야 하며 최대8번까지만 전달; 보관량 상한 없음; operation ID·ObjectGeneration·payload·reply route 유지; 순환은 Unavailable, generation불일치는 InvalidOperation | 21 §6.3 (872-876행) | |
| R80 | 운영도구 위치조회는 배치조건으로 안씀; ID조회는 record없으면 empty(Missing entry 안만듦); Paged list(object kind필수, stable type·MeshName선택), 한페이지 1..1000개, 최대4 MiB, 항목당 전역ID·ObjectGeneration·MeshName·Node RID·상태·stable type; continuation token은 opaque(같은cycle 내 ID중복 안됨); 전체 무제한 반환함수 없음 | 21 §6.4 (880-893행) | |
| R81 | 조회결과 상태표(record없음=empty / Creating=entry포함 / Ready=entry포함 / Commit뒤 현재owner 사용불가=Unavailable entry / Store조회실패=Unavailable Framework error, page전체 error로 끝냄); Topology 열거는 MeshNode descriptor만 대상(ClientServer·fanout채널은 없음, 24 §2.2로 확인) | 21 §6.4 (896-908행) | |
| R82 | 이 절 소유범위=Location Store record·generation·target-only CAS조건만(Actor·Spot 공통흐름은 28 소유); relocation은 source·target process 실행중에만 진행 | 21 §7 (913-949행) | |
| R83 | RelocationId(0아닌128-bit난수)·TargetAttemptGeneration(0아닌값, 항상 exact equality로만 대조, target node lifecycle generation에서 유도금지)·Reservation ID(0아닌128-bit, 생성용ID와 별개) | 21 §7.1 (955-957행) | |
| R84 | 위치record 최대1 MiB(큰목록은 분할, 완료기록payload는 Relocation Store); target 공간확보 시 고정값(object ID·StoreVersion·종류·type·source·target실행세대·owner정보·필요공간); 확인결과별 처리표(일치=계속 / source lease만료=자동이어받지않음 / target모두유효=확보 / 같은ReservationID+같은내용=재반환 / 다르거나만료=Conflict) | 21 §7.1 (959-983행) | |
| R85 | SpotWide 전체이동은 2종 Store변경만 허용(새owner로이동 / 완료후진행정보제거); 목적 안맞으면 Conflict; 준비성공 시 (AggregateId,AggregateGeneration)+Prepared기록(같은요청 AlreadyPrepared, 다른요청 Conflict); 마지막변경은 목록시작위치·전체항목수·내용확인값 재검사, 한번의 CAS로 전체가 새owner 따름 | 21 §7.1 (989-1003행) | |
| R86 | PerActor User Spot은 Spot authority와 Actor owner 분리변경(target shell준비→current turn·Create·Join완료→같은SpotId·ObjectGeneration 유지한 채 Spot authority만 CAS); Member Actor는 각자 독립unit(source 남은Actor는 독립준비, 합계가 전체membership수와 일치확인), 마지막Actor+relay종료 후 Completed기록 | 21 §7.1 (1005-1015행) | |
| R87 | source→target owner CAS는 target만 실행(source·Session owner는 Location Store 안씀); CAS는 Restore+temporary queue등록완료+cutover수신 또는 1,000ms 경과 전엔 시작 안 함 | 21 §7.2 (1019-1022행) | |
| R88 | 단계별owner 인정표(Preparing·Captured=source / Prepared=source(target시도번호+lease+node필요) / Committed~Completed=정확히기록된target); membership불변 Actor이동은 NewOwner CAS 1회; 같은target process 재시도 시 target시도번호+준비정보만 교체(이전시도는 owner 못바꿈) | 21 §7.2 (1024-1032행) | |
| R89 | 이동종류별 Location Store 함께바꾸는값 표(Actor하나 host relocation / Cross-node JoinSpot·JoinEntrySpot / SpotWide host relocation / PerActor authority전환 / PerActor member이전, 각각 다른값조합); ObjectGeneration은 유지, AuthorityOwnerGeneration만 증가; SpotWide 완료전엔 일부object만 target조회 불가, PerActor는 Actor별 조회 허용(그 operation 안에서만) | 21 §7.2 (1034-1046행) | |
| R90 | SpotWide 이동대상수 상한없음(§1.2 페이지제한); Actor 하나라도 조건불만족 시 state저장 전 전체거부; Target factory·Restore는 Prepared기록 전 완료 | 21 §7.2 (1048-1053행) | |
| R91 | Restore대화가 공식snapshot 확정(전체길이·chunk수·CRC-32C checksum 선언); 요청+각chunk+CAS는 같은RelocationId+target attempt+source fence로 결합; 정확히 같지않은 Restore·chunk는 조립폐기(늦은attempt payload 안섞임) | 21 §7.3 (1066-1071행) | |
| R92 | 단계별 Location Store 기록값·다음단계조건표(Preparing·Captured·Prepared·Owner변경·Completed 5행) | 21 §7.3 (1073-1079행) | |
| R93 | Target은 결합값 같은chunk만 조립; checksum·목록확인값 불일치 시 복원·owner변경 시작 안하고 명시적실패(부분조립payload로 복원안함); 같은RelocationId라도 target attempt 다르면 이전조립상태 미사용; 결합값 같은 Restore 재전송은 선언길이·checksum이 처음값과 같을때만 재사용, 다르면 명시적conflict실패 | 21 §7.3 (1112-1119행) | |
| R94 | Authority commit(CAS)은 row 자신의 identity(reservation id+AuthorityOwnerGeneration+target attempt)만 fence, target node liveness·lifecycle generation은 여기서 검증안함; 일치identity fence 아래 성공commit은 target node 생존여부와 무관하게 authoritative | 21 §7.3 (1121-1126행) | |
| R95 | 실패시점별 처리표(Preparing·capture중 source종료=취소 / Captured뒤 relay-ready전 target명시실패=취소+source owner유지+memory payload로 queue복원 / 조립checksum불일치=명시적실패 재시도안함 / Captured·Prepared직전 필수정보소실=owner불변 취소) | 21 §7.3 (1128-1133행) | |
| R96 | Captured전 요청실패는 일반 connection failure로 처리(다른node 자동실행보장 안함); Captured뒤는 source memory payload를 정상handoff 근거로 사용(process 재시작후 자동복구엔 미사용) | 21 §7.3 (1135-1138행) | |
| R97 | Target은 object ID+예상AuthorityOwnerGeneration으로 현재위치 직접읽음; record없거나 generation다르면 factory·복원준비 시작안함; network command에 위치정보 전체 복사안함 | 21 §7.3 (1140-1143행) | |
| R98 | Target은 factory·Restore중 새message를 temporary queue에 보관(handler 미전달); 같은target process내 Restore 재시도 시 실패instance 버리고 새instance; callback은 같은입력 재수신해도 상태불변 보장, 정확히 한번 실행보장은 안함; target process종료 시 다른runtime이 자동재개 안함 | 21 §7.4 (1147-1152행) | |
| R99 | Ready 5조건(owner·membership변경완료 / 미완료작업·timer복원완료 / saved work+relay+temporary work를 순서대로 실제queue삽입 / temporary queue등록제거+regular route로 atomic전환 / lifecycle callback완료+dispatch열기); Source ingress hold원본제거·위치record Completed변경·command 44적용은 target message처리를 막지않음 | 21 §7.4 (1154-1164행) | |
| R100 | Resolver는 5조건 모두만족 전엔 이동중object를 Ready로 반환안함; PerActor target shell은 Spot authority CAS+source relay+target admission준비완료 시 Ready(member전체이전 안기다림), Actor direct resolve는 각Actor의 current owner·Ready상태 사용 | 21 §7.4 (1166-1174행) | |
| R101 | OperationId(중복처리방지)·Source request정보(OwnerId·LeaseGeneration·Node RID·node세대)·ReplyRouteId(send·event엔 없음)·저장한 완료결과key(RelocationId+source request정보+OperationId); 이값들은 같은 source host실행중 0아니며 재사용안함(값소진 시 runtime오류) | 21 §7.5 (1178-1186행) | |
| R102 | Framework는 object순서로 완료결과 저장(중복저장 안함); 새완료결과+payload를 Relocation Store에 먼저저장 후 Location Store의 payload위치·checksum·완료수·전달대기수를 CAS 한번 변경; 수락request수=완료결과수 그리고 전달대기수0일때만 Completed기록(다르면 복구오류) | 21 §7.5 (1188-1194행) | |
| R103 | 완료확인 근거3종(TerminalReceived·AlreadyTerminal·SourceLeaseExpired); connection종료·재연결만으로 완료판단 안함; source owner lease유효한 채 Relocate deadline넘으면 ForceStopped, payload+reply bytes 24시간 보관 | 21 §7.5 (1196-1205행) | |
| R104 | Relay-ready accepted전 취소 6단계(source새작업 안받는상태유지 → target temporary queue작업폐기+source ingress hold원본·기존작업 원래순서로 되돌림 → bound Session seal있으면 command 44 abort를 one-way전송(reply안기다림) → 확보target공간+조립중chunk정리 → Location Store읽지도쓰지도않고 source owner·generation·수용공간유지(이동진행정보만 제거) → source가 새작업 다시받음); Session owner는 matching seal만 해제하고 held를 source route로 제출 | 21 §7.6 (1209-1224행) | |
| R105 | Store결과 못받으면 성공·실패 추측안함(같은key+처음읽은version으로 재확인); cutover·Session route update는 one-way라 target→source completion reply없음, source가 대신 Location Store갱신 안함; relocation CAS retry deadline=Restore operation의 absolute deadline | 21 §8 (1228-1236행) | |
| R106 | Retry가능failure·불확정응답이면 같은source fence+RelocationId로 반복(exact target owner확인 시 성공수렴, 다른valid owner·generation이면 즉시stale종료); Restore유효시간까지 못확인하면 location_update_failed Error(준비된instance·temporary queue·relocation state 제거, dispatch안열고 Session route update안보냄); 이미terminal된 RelocationId의 늦은Store응답은 재활성화 안함 | 21 §8 (1237-1244행) | |
| R107 | StoreFailureGrace동안 마지막완전읽은 descriptor목록 유지(새outbound connection안만듦); grace는 owner lease·relocation deadline 연장안함; §4시각넘으면 차단, 복구시 owner·descriptor전체 재확인 | 21 §8 (1246-1254행) | |
| R108 | Provider요청 시작전 cancellation은 Store호출 자체 안할수있음; 시작후 cancellation·timeout·provider error는 변경여부불명, 같은key+예상StoreVersion 재읽어확인 | 21 §8 (1256-1259행) | |
| R109 | Relocation Store쓰기는 같은reference로 재읽거나 재저장 가능해야함; provider가 입력bytes를 완료뒤에도 보관하려면 복사해야함, 성공결과bytes는 이후 불변 | 21 §8 (1261-1264행) | |
| R110 | Relocation Store저장은 먼저저장+재확인 후에만 Location Store가 reference가리키도록 CAS; 삭제 시 Location Store에서 reference사용종료 먼저기록 후 payload삭제; 두Store는 2PC로 안묶음(다른Redis에 둘수있음) | 21 §8 (1266-1272행) | |
| R111 | Location Store가 가리키는 payload가 일시적으로 안보이면 제한된횟수 재읽고 위치record도 재확인; 영구없거나 checksum다르면 DataLost(owner·membership을 source로 되돌리거나 다른payload추측 사용안함) | 21 §8 (1274-1278행) | |
| R112 | Host종료 시 descriptor·owner lease 삭제후보는 같은시점 목록읽기로 찾고, key재읽어 처음version 그대로일때만 삭제; 위치record는 명시적Delete로만 제거(descriptor 사라짐만으론 삭제안함) | 21 §9 (1286-1291행) | |
| R113 | Owner cleanup sweep(removeAllByOwner)은 authority row만 회수(descriptor는 절대회수 안함, descriptor는 자신의 lease만료·TAKEOVER로만 회수, 두 정리경로는 독립); Deadline넘으면 ForceStopped결과 1회완료, timer·callback·observer는 Framework소유 resource보다 오래남으면 안됨 | 21 §9 (1293-1301행) | |

### 5.2 22-location-store-redis

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R114 | Provider는 Framework가 만든 opaque key·bytes 저장; 여러key 조건검사·변경을 하나의commit으로 적용; 복구용 snapshot은 page크기 제한 제공 | 22 §1 (15-17행) | |
| R115 | Provider는 Actor·Spot authority·owner lease·placement·aggregate·relocation phase 의미를 몰라도 됨(21이 방법·등록조건 소유); Application은 SPI 직접호출 안함(provider package만 구현) | 22 §1 (19-24행) | |
| R116 | SPI는 3개 operation군만(Exact read·Conditional atomic batch·Snapshot scan) | 22 §2 (28-34행) | |
| R117 | SPI type은 기본Framework API와 분리된 provider abstraction package 소유; 기본package는 abstraction 의존하되 Store operation을 application API로 재노출 안함; 외부provider는 application·Actor·Spot package 미의존 | 22 §2 (36-38행) | |
| R118 | Descriptor·authority·reservation·capacity·aggregate·lease·change-stamp별 public method·DTO 추가 안함; Redis command·key layout·script·private encoding도 SPI에 노출 안함 | 22 §2 (40-41행) | |
| R119 | IZLinkLocationStore = ReadAsync·WriteAsync·ScanAsync 3메서드만 | 22 §2 (46-61행) | |
| R120 | Key=Framework발급 opaque UTF-8 1..1024 bytes, case-sensitive exact match; Value=최대1 MiB(commit뒤 변경안됨, expiry없으면 explicit delete까지 유지); Version=provider발급 opaque UTF-8 1..4096 bytes(Framework 미해석) | 22 §3 (74-76행) | |
| R121 | StoreNow=read·commit·scan page 기준 provider wall clock; Exact read는 Missing(StoreNow)·Found(bytes,version,optional expiry,StoreNow) 반환(만료된value는 Missing); provider는 read결과 사용중 bytes변경·재사용 안함 | 22 §3 (77-81행) | |
| R122 | Framework domain generation은 provider version과 다름; provider는 domain counter 해석·별도generation API 제공 안함 | 22 §3 (83-84행) | |
| R123 | Write request=condition집합(Missing(key)·Version(key,expected)·Put(key,bytes,optional retention)·Delete(key))+mutation집합; 모두참일때만 하나의commit으로 적용, 다른caller는 중간상태 관찰불가; 하나라도 거짓이면 Conflict(mutation·version증가 0, 실패condition·현재value 미반환) | 22 §4 (90-97행) | |
| R124 | Batch bound(unique key합계 최대2,048개, encoded request 최대4 MiB, 같은key를 condition·mutation에 두번 미사용); Applied는 각Put의 opaque version+commit 관측StoreNow 반환 | 22 §4 (99-104행) | |
| R125 | User Spot participant 전체를 batch하나에 안넣음; Framework가 최대1,024개+1 MiB로 제한한 immutable inventory chunk를 미리저장; 마지막batch엔 aggregate authority·inventory root·count·digest·capacity counter처럼 작은record만 | 22 §4 (106-109행) | |
| R126 | 한 User Spot에 속할수있는 Actor총수는 batch의 2,048-key 제한으로 안정함; provider는 inventory chunk·participant·aggregate 의미해석 안함 | 22 §4 (111-112행) | |
| R127 | Prefix=UTF-8 0..1024 bytes(key와 같은 exact comparison); 첫page요청엔 cursor없음, provider가 고정snapshot 만들고 다음page 있으면 opaque cursor반환; Page limit 1..1000, 최대4 MiB; Cursor=opaque UTF-8 1..4096 bytes | 22 §5 (118-123행) | |
| R128 | Snapshot이 더이상 없거나 cursor 무효면 Expired반환; Framework는 Expired시 이전page결과 버리고 첫page부터 다시읽음(scan item은 복구후보일뿐, mutation전 exact read+expected version 재확인) | 22 §5 (124-127행) | |
| R129 | Cursor encoding·snapshot 보존구조·Redis SCAN 사용여부는 provider implementation detail | 22 §5 (129행) | |
| R130 | Operation시작전 cancellation은 I/O·commit시작을 막음; 시작뒤 cancellation·timeout·transport error는 commit여부 불명확할수있음(provider는 성공·Conflict로 추정안함, Framework가 exact read+expected version으로 재구성) | 22 §6 (133-135행) | |
| R131 | 입력bound위반·같은key 중복지정은 언어별 argument validation error; Missing·Conflict·Expired는 정상closed result, provider-specific failure는 Framework가 Store failure로 분류가능해야함(Redis command·key layout·script정보는 public API 비노출) | 22 §6 (137-140행) | |
| R132 | Input bytes는 operation 끝날때까지 불변(provider가 그뒤 보관하려면 복사), success result bytes는 consumer 사용중 stable | 22 §6 (142-143행) | |
| R133 | Provider instance 등록조건·root소유권은 21의 등록을 따름; Store사용 runtime·background operation 모두 끝난뒤 정확히 한번 dispose; 여러Store가 connection공유시 중복dispose 방지책임은 provider | 22 §7 (147-150행) | |
| R134 | 공식Redis extension package는 언어별 naming convention의 `RedisLocationStore` 구현제공, 공개options는 connection·key namespace·operation timeout으로 제한 | 22 §7 (152-153행) | |
| R135 | Framework generation counter logical key표(`zlink:v11:owner-counter`=OwnerLeaseGeneration(불변) / `zlink:v11:object-counter`=ObjectGeneration / `zlink:v11:authority-owner-counter`=AuthorityOwnerGeneration); 각value는 sign·leading zero·envelope없는 bare UTF-8 canonical decimal | 22 §7 (159-165행) | |
| R136 | 행없으면 다음값1; v발급시 CAS로 v+1 Put; n개묶음 발급시 v..v+n-1 발급후 v+n Put; 저장가능범위 1..2^63-1(0은 저장·발급안함), 2^63-1 저장행은 소진상태(typed GenerationExhausted 반환, record·counter 불변), 최대발급값 2^63-2 | 22 §7 (166-170행) | |
| R137 | Counter mutation은 그값이 gate하는 record와 같은 conditional write batch(하나의 EVAL)에 반드시 포함; provider mapping은 모든 logical counter를 자동으로 같은 `{zlink-location-v3}` hash slot에 둠; counter logical key는 `authority\0`·descriptor scan-preimage prefix 밖에 둠 | 22 §7 (172-176행) | |
| R138 | 운영 clean break — Store마다 한번, retired logical literal 5개(각각 SHA-256으로 계산한 physical opaque key)를 flush(literal마다 다시계산, prefix에서 추측안함) | 22 §7 (178-182행) | |
| R139 | 5개 record는 `{prefix}:{zlink-location-v3}:opaque:{sha256hex(preimage)}` 저장방식을 반드시 따름({prefix}=provider등록시 지정namespace, preimage=21 §2.4 정의) | 22 §7 (184-191행) | |
| R140 | `{zlink-location-v3}` 중괄호는 Redis Cluster hashtag(Put이 record·counter·index를 같은script에서 함께바꾸므로 domain전체를 한hash slot에 고정해야 multi-key EVAL이 원자적; 중괄호빼면 slot이 흩어져 원자성 깨짐) | 22 §7 (191-194행) | |
| R141 | 자료구조=Redis ZSET; Put마다 provider쪽 단조증가 INCR counter를 score로 붙여 append-log 기록(가장큰 score member가 현재값); member value는 {originalKey,rawBytes,version,expiresAtMs,tombstone}을 담은 cmsgpack array 앞에 1-byte format tag 0x01(rawBytes는 base64 재인코딩없이 원본그대로) | 22 §7 (195-199행) | |
| R142 | 인식못하는 format tag 만나면 명시적실패(값추측금지); cmsgpack member의 MessagePack type표(originalKey·rawBytes=`str` family, version=`str` family, expiresAtMs=부호없는 `int` family(0=만료없음), tombstone=`bool`); 바깥array=`array` family(fixarray 5요소) | 22 §7 (200-216행) | |
| R143 | 5개record와 opaque record표현 제외하고 나머지는 Redis provider implementation detail(Lua script·transaction분할, connection lease·retry·snapshot cursor구현, change stamp·polling최적화); Redis provider도 §4 generic batch·§5 snapshot scan을 그대로 지원(Authority DTO·change-stamp capability interface는 비공개) | 22 §7 (218-226행) | |
| R144 | Redis에 이미저장된 옛key·value형식을 새opaque record로 변환하는 하위호환경로 없음(이형식 배포는 clean break, 기존Redis상태는 draining하거나 유실감수); format tag·recordVersion이 다른버전 혼재시 조용히 선택 안하고 명시적실패; Location Store·Relocation Store는 같은Redis deployment에서 다른key namespace 사용 또는 물리분리 가능(connection공유·cross-store transaction에 correctness 의존 안함) | 22 §7 (228-234행) | |

### 5.3 23-relocation-store-redis

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R145 | Provider개발자는 Framework가 만든 reference를 그대로 key로 payload저장, 같은reference로 읽기·보존기간연장·삭제; handoff payload(state·queue·timer)는 이provider 미거침(source가 target에 직접전송, 규칙은 28 소유) | 23 §1 (19-25행) | |
| R146 | Application은 SPI 직접호출 안함; Relocation Store는 authority(owner·lifecycle관리) 안함, Location Store 게시전 payload와 이미게시된 payload만 reference로 보관; provider는 bytes의 업무의미 해석 안함(activation envelope·완료된request의 reply payload·완료결과·그 reference목록도 해석없이 저장) | 23 §1 (27-39행) | |
| R147 | Cold activation·Actor Join의 실행절차는 이문서 범위 아님 | 23 §1 (40-42행) | |
| R148 | SPI는 4개 operation만(Put·Read·Renew·Delete); SPI type은 provider abstraction package 소유(21·22와 동일원칙) | 23 §2 (48-58행) | |
| R149 | 단계·manifest·participant·replay cursor·completion별 public method·DTO 추가 안함(이동이력조회 operation도 없음, 관찰은 25 지표+26 tracing 담당); SPI operation type·DTO는 추상적(chunk저장구조·script는 비노출); **공식Redis provider가 사용하는 Redis key배치·data type 자체는 §8이 MUST-level로 고정하는 공개계약**(언어간 상호읽기 위해) | 23 §2 (60-67행) | |
| R150 | IZLinkRelocationStore = PutAsync·ReadAsync·RenewAsync·DeleteAsync 4메서드 | 23 §2 (73-97행) | |
| R151 | Reference=Framework가 Put전 발급 opaque UTF-8 1..4096 bytes(대소문자구분, 전체값 정확일치 비교); Application data chunk=Framework가 나누기전 기준 최대64 MiB; Redis encoded blob=data chunk+immutable envelope 합친입력, 공식Redis provider 최대 64 MiB+23 bytes | 23 §3 (113-115행) | |
| R152 | 여러blob으로 나눈 payload 전체 최대256 GiB; Chunk수는 맨앞목록이 가리키는 data chunk합계 최대4,096개; StoreNow=Put·Read·Renew결과에 담는 현재시각 | 23 §3 (116-118행) | |
| R153 | Provider는 reference를 만들거나 바꾸지않음(같은content라도 Framework가 다른reference 지정하면 별개value); 삭제·만료된 reference를 다른bytes에 재사용 안함 | 23 §3 (124-125행) | |
| R154 | Framework는 64 MiB초과 payload를 최대64 MiB data chunk로 분할, 각chunk에 checksum+23-byte immutable envelope; 별도 맨앞목록에 format version·전체길이·checksum·chunk순서·각chunk의 reference·길이·checksum 기록(provider는 이목록도 일반bytes로 저장) | 23 §3 (127-133행) | |
| R155 | 저장 시 모든data chunk를 다시읽어 bytes+checksum확인한 뒤에만 맨앞목록 저장(일부chunk 저장·확인실패 시 맨앞목록 저장안함, 남은chunk는 retention만료로 정리) | 23 §3 (135-137행) | |
| R156 | 읽을때는 각data chunk checksum확인 후 맨앞목록 순서로 합침(합친전체 checksum도 일치해야 사용); chunk 하나라도 없거나 checksum다르면 전체를 DataLost(부분chunk만 사용안함); 보관기간 연장시 각chunk 확인후 모두성공해야 맨앞목록 연장 | 23 §3 (139-144행) | |
| R157 | 각data chunk·맨앞목록 기본retention 24시간; Framework는 남은retention 12시간 시점을 기본 renew threshold로 사용; provider clock으로 expiry 계산(application host wall clock 미사용) | 23 §3 (146-148행) | |
| R158 | Put(reference,payload,retention) 3결과(Stored·AlreadyStored·Conflict); provider는 payload전체를 byte단위 비교(같은content에 새reference 발급·provider선택 reference반환 API없음) | 23 §4.1 (154-161행) | |
| R159 | Read(reference)는 만료안된 payload있으면 Found(bytes,expiresAt,storeNow), 없거나 만료시 Missing(storeNow); Found bytes는 consumer 사용중 변경·재사용 안됨 | 23 §4.2 (165-168행) | |
| R160 | Renew(reference,retention)는 provider clock기준 새expiry 계산, payload있으면 Renewed(expiresAt,storeNow), 없으면 Missing; 반복해도 payload bytes 불변 | 23 §4.3 (172-174행) | |
| R161 | Delete(reference)는 reference 없을때도 성공하는 idempotent operation(여러번 실행해도 최종상태 동일) | 23 §4.4 (178-179행) | |
| R162 | Operation시작전 cancellation은 I/O·write시작 안함; 시작뒤 cancellation·timeout·transport error는 저장·삭제 적용여부 불명확(성공추정 안함) | 23 §5 (183-185행) | |
| R163 | 공식Redis provider의 OperationTimeout은 connection획득+command완료를 합친 operation전체 적용(초과시 DeadlineExceeded로 변환, 이미전달한 write는 timeout뒤에도 적용될수있어 실패로 추정안함) | 23 §5 (187-190행) | |
| R164 | Put결과 못받으면 발급한reference로 Read 실행하거나 같은reference·bytes로 Put 재실행하여 저장여부 재구성가능해야함; Delete는 재실행해도 같은상태, Renew도 payload 불변 | 23 §5 (192-194행) | |
| R165 | 입력계약위반(reference길이·payload크기·retention)은 언어별 argument validation error; Missing·AlreadyStored·Conflict는 정상결과(그외 provider-specific failure는 Store failure로 분류가능해야함, Redis정보는 public API 비노출) | 23 §5 (196-199행) | |
| R166 | Caller가 넘긴 input bytes는 operation 끝날때까지 불변(provider가 완료뒤에도 참조하려면 먼저 복사) | 23 §5 (201-202행) | |
| R167 | Location Store authority가 아직 안가리키는 payload=orphan(게시전 작업중단 시 retention만료 뒤 제거); published reference는 Location Store에서 사용종료를 먼저 commit한 뒤 payload 삭제(provider는 retention남은 published payload 임의삭제 안함) | 23 §6 (206-213행) | |
| R168 | Provider instance 등록조건·root소유권은 21의 등록을 따름(정확히 한번 dispose); 여러Store instance가 connection공유가능, dispose시 연결해제·중복방지 책임은 provider | 23 §7 (217-223행) | |
| R169 | 공식Redis extension package는 언어별 naming convention의 `RedisRelocationStore` 제공, 공개options는 connection·key namespace·operation timeout | 23 §8 (227-228행) | |
| R170 | Payload는 Redis raw-bytes STRING 저장; key=`{prefix}:{zlink-relocation-v1}:blob:{reference}`; 이key형식은 22 §7의 Location Store opaque record와 독립된 별도버전 domain tag(`zlink-relocation-v1`) 사용(두Store가 같은Redis 공유해도 key공간 안겹침) | 23 §8 (230-235행) | |
| R171 | `{zlink-relocation-v1}` 중괄호는 Location Store와 같은이유의 Redis Cluster hashtag(relocation blob domain전체를 한hash slot에 고정) | 23 §8 (235-240행) | |
| R172 | retention은 Redis PSETEX 또는 SET의 PX option으로 적용(Renew는 같은key에 새PX 재설정) | 23 §8 (240-242행) | |
| R173 | Redis provider 내부구현(public contract 아님) 4항목(chunk저장 보조자료구조, script·private serialization record, connection lease·cleanup index, retry·cleanup 내부방식) | 23 §8 (244-249행) | |
| R174 | Redis 전용 등록helper나 두Store를 함께구현하는 결합class 제공안함(21 §1.3·22 §7과 동일원칙) | 23 §8 (251-252행) | |
| R175 | 옛key형식→새 `zlink-relocation-v1` 형식 하위호환경로 없음(clean break, 기존Redis상태는 draining하거나 유실감수) | 23 §8 (254-256행) | |
| R176 | Location Store와 Relocation Store는 같은Redis deployment에서 다른namespace 또는 서로다른 deployment에 둘수있음(connection공유·묶은transaction에 correctness 의존안함) | 23 §8 (258-260행) | |

### 5.4 28-relocation-flow + 44-internal-relocation-continuity + 52-internal-relocation-handoff (병합 대상)

44·52의 규칙 대부분이 28과 동일 사실의 재서술이다(§3.1). 아래 표는 28을 기본 축으로 삼고, 44·52가
같은 사실을 반복하는 곳은 "동일" 열에 근거를 남기며, 44·52에만 있는 net-new 내용은 별도 행으로
추가한다.

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R177 | Owner change는 single, source→target 한번 Location Store 경유; Graceful만 지원(process종료 중 자동인계 없음) | 28 §1 (23-31행) | |
| R178 | Relocation unit표(Entry-Spot Actor=Actor 1개 / PerActor Spot=authority+각member Actor / SpotWide Spot=Spot+전체member / Instance Spot=1개) | 28 §3 (53-58행) | |
| R179 | Entry Spot 자체는 relocation 안함(node lifecycle) | 28 §3 (60-68행) | |
| R180 | ObjectGeneration 보존, AuthorityOwnerGeneration이 owner변경 순서 구분 | 28 §3 (70-71행) | |
| R181 | Exact identity=RelocationId+targetAttemptGeneration(target-prepare 시도별 0아닌 고유값)+coordinator fence; handoff가 소유하는 값 인벤토리(object identity, source/target fence, relocation identity, saved-work reference, relay connection, temporary queue) | 28 §3 (72-76행), **52 §2 표와 동일**(52 58-66행) | |
| R182 | Target preflight 먼저(미지원target이면 relocation 미시작, source영향없음); Session binding seal은 source dispatch 정지전(→세션문서 §8, §4 S1) | 28 §4.1 (84-90행) | |
| R183 | Source는 현재turn 종료후 queue+timer+state를 하나의 payload로 capture, memory에만 유지(cutover submit terminal+resend window까지); direct-transfer schema=`relocation-envelope-v1`(canonical big-endian, provider envelope없음, field순서: relocation·object·applicationVersion·applicationStates·savedWork·timerRegistrations·pendingTimerTicks) | 28 §4.2 (94-102행), **52 §3.1과 동일**(52 76-90행) | |
| R184 | participantId=1+UTF-8정렬된 authority-key inventory의 0-based index(participant vector는 정렬·고유); savedWork=(participantId,order,record) 고정vector; timerRegistrations·pendingTimerTicks 필드구성 | 28 §4.2 (103-115행) | |
| R185 | Command 40 relocationPrepare=manifest(payloadTotalLength·payloadChunkCount·payloadChecksumCrc32c); Command 52=chunk전송(같은ordered connection, 경계임의분할, 다른object message interleave 가능) | 28 §4.2 (118-124행) | |
| R186 | Effective chunk size=min(RelocationPayloadChunkLimit 기본256 KiB, target 사전seal advertised receive cap, §5.3 in-flight budget); negotiation reply 없을때 fallback 32 KiB | 28 §4.2 (126-130행) | |
| R187 | Captured queue·timer는 source가 재relay 안함; source는 target의 명시실패reply(relay-ready 전)에만 memory사본으로 복원; mailbox drain 안기다리고 Restore 전송후 ingress hold에 신규message 버퍼 | 28 §4.2 (132-139행) | |
| R188 | Target은 Restore payload 도착전 temporary queue 먼저등록(Restore중 직접도착 message는 handler 미전달, temp queue로); 각chunk를 조립buffer에 복사즉시 Core retained lease 해제; 전체조립후 checksum 확인후 factory 실행(불일치시 명시적실패reply, 재시도·부분복원 없음) | 28 §4.3 (143-155행), **52 §4.1과 동일**(52 135-153행) | |
| R189 | Chunk·Restore routing은 exact identity로만 keying(불일치시 폐기); 같은exact identity라도 길이·checksum 다른 늦은Restore요청은 명시적conflict실패(재사용·덮어쓰기 없음) | 28 §4.3 (157-160행) | |
| R190 | Temporary queue·saved-work backlog는 공유예약된 Application Job Queue permit 사용, durable handoff 직후 반환(runnable아닌 backlog는 live permit 안붙듦) | 28 §4.3 (165-169행), **44·52와 동일**(44 106-109행, 52 144-147행) | |
| R191 | "relay 수신준비 완료" reply는 target측 retained-byte owner가 모든 staged payload에 고정된 뒤에만 전송(relocation완료 뜻 아님, owner불변) | 28 §4.3 (171-174, 179-180행), **52 153행 동일** | |
| R192 | Source는 relay-ready reply뒤 ingress-hold-accepted message만 같은TCP connection으로 relay | 28 §4.4 (184-186행), **52 §3.2 동일**(52 97-112행) | |
| R193 | Cutover=one-way [send](RelocationId+boundary relay record수+그relay의 CRC-32C, reply없음) | 28 §4.4 (187-192행) | |
| R194 | **Relay-ready-accepted는 비가역경계**; source는 cutover 정확히 1회제출(submit terminal까지 queued-job permit 유지, terminal뒤 resend window(=cutover wait timeout길이)까지 payload retained-byte owner 유지후 정확히1회 정리, §5.3 in-flight budget 불포함) | 28 §4.4 (194-199행), **52 §1 결정과 동일**(52 45-48행) | |
| R195 | Target completion reply는 cleanup gate 아님; accepted뒤 cutover-submit 실패는 source복원 안함(target은 cutover-wait fallback으로 진행) | 28 §4.4 (200-203행) | |
| R196 | Target은 cutover선언 record수·checksum을 수신relay와 대조(불일치=Error, 구현결함이지 정상상황 아님) | 28 §4.4 (206-209행) | |
| R197 | **RelocationCutoverWaitTimeout 기본1,000ms**(relay-ready-reply→cutover도착 측정) | 28 §4.4 (211-212행), **44 112-115행·52 176행과 동일값(30도 §8 572-573행에서 동일값 반복)** | |
| R198 | Connection유실 시 source는 전체boundary batch+cutover를 새connection으로 재전송(target은 부분수신 폐기하고 전체재전송으로 원자적 교체, 병합·중복제거 아님) | 28 §4.4 (213-217행) | |
| R199 | Timeout에서 resend 없으면 target은 cutover_timeout Warning 기록하고 CAS·queue open 진행(순서없는 fallback); fallback후 late·duplicate cutover는 late_cutover Warning만(상태불변) | 28 §4.4 (219-222행), **52 176-177행 동일** | |
| R200 | 경계뒤 늦은message는 owner변경전=temp queue로 relay, owner변경후=Message Follow | 28 §4.4 (224-225행), **52 114-116행 동일** | |
| R201 | Target CAS 4개 선행조건(factory+Restore완료 / temp queue등록 / cutover수신 또는 1,000ms경과 / current owner·ObjectGeneration·owner-generation·membership이 first-read source값과 일치); CAS는 owner+membership을 하나의 atomic op으로 변경(불일치시 무변경, target queue closed 유지) | 28 §4.5 (227-239행), **52 §4.2 CAS evidence checklist와 동일**(52 157-174행) | |
| R202 | 일시적Store error·불확정응답은 같은 expected fence+RelocationId로 retry(retry deadline=Restore 자체 absolute deadline, 재설정·연장 안함); 응답불명이면 먼저 재읽어 exact target owner 이미 commit됐는지 확인 | 28 §4.5 (241-245행) | |
| R203 | Deadline초과+target owner 미확인=relocation 실패(location_update_failed Error, target이 준비된 Actor·Spot instance·temp queue·relocation state 제거, queue안열고 Session route update 안보냄; terminal RelocationId의 늦은Store응답은 재활성화 안함) | 28 §4.5 (247-252행) | |
| R204 | Store가 다른valid owner·generation을 드러내면 deadline대기 없이 즉시 stale-relocation 종료 | 28 §4.5 (250-252행) | |
| R205 | Actor unit은 준비된 target Actor만 제거; Spot unit은 준비된 Spot scope+staging member Actor 제거; Source application execution은 relay-ready accepted뒤 재개 안함(Message Follow는 일정대로 종료); Source·Session owner는 Location Store 변경을 직접 안함 | 28 §4.5 (254-260행) | |
| R206 | CAS성공 후 target은 backlog순서 3단계 확인(pre-capture queued work·timers → pre-boundary relay → post-boundary temp-queue work) | 28 §4.6 (264-269행), **52 §4.3 동일순서**(52 185-195행) | |
| R207 | Post-CAS는 temp route→regular dispatch route 전환, lifecycle callback 실행; runnable turn마다 공유 queued-job permit을 하나씩 획득(사전대량예약 아님, 실제handler 시작이 permit 반환); 대기항목은 backlog retained-byte owner가 계속 소유 | 28 §4.6 (270-274행) | |
| R208 | Timer는 native lifecycle 유지, runnable해지면 일반ingress 규칙 따름; CAS성공뒤 source에 완료reply 없음(bound Actor면 Session owner에 one-way route·held-message·seal-release 통지, §4 S1) | 28 §4.6 (275-279행) | |
| R209 | Source는 target완료reply 안기다리고 실행종료, late old-address traffic을 Message Follow로 전환(cutover-timeout fallback에서도 동일규칙) | 28 §4.6 (283-285행) | |
| R210 | Session owner의 route 적용·seal 해제 시점·timeout·경쟁·abort 처리 전체(seal, held message, route 전환, command 42/43/44) | 28 §4.7 전체(281-303행), 44 91-94·138-142행, 52 §5 전체·mermaid·§6·§7(197-267행), 30 여러위치 | **세션문서 `04-session/02-session-actor-binding.ko.md` §8 링크(§4 S1·S17 표 참조), 04는 요약 3~4문장만** |
| R211 | 보장하는 순서(§5.1) — 도착 order, correlation 유지 등 | 28 §5.1 (405-415행) | |
| R212 | `send`는 target identity+payload만 보존(caller는 transport submit결과만 기다림, app response없음); `request`는 operation identity·correlation·reply route·payload·deadline 보존(caller는 target response나 원래timeout으로 대기); relocation은 send-ACK 추가 안함, request를 새operation으로 재생성하거나 숨은 다른target에 재제출 안함 | 28 §5.2 (417-424행), **52 §3.3 동일표**(52 120-129행) | |
| R213 | 동시unit·participant·relay record수에 relocation전용 correctness 상한 없음(일반runtime·frame·Store 제한은 그대로 적용) | 28 §5.3 (431-435행), **44 49-51행 동일** | |
| R214 | RelocationInFlightPayloadBudget=source-node당-peer-connection당 concurrent chunk byte합, 기본16 MiB(0=무제한); RelocationNodeInFlightPayloadBudget=whole-source-node concurrent chunk byte합, 기본0=무제한 | 28 §5.3 (442-443행) | |
| R215 | Budget-full이면 새unit은 source admission seal 적용전 대기(downtime에 안셈); in-flight unit의 다음chunk는 headroom 대기; 이미시작한 relocation은 budget으로 실패 안함 | 28 §5.3 (445-451행) | |
| R216 | 회계기준=Core accounted charge(frame metadata+payload, raw encoded bytes 아님), chunk submit시 추가·Core 충전해제 보고시 차감; observable API 이전시대=min(고정보수적 per-pipe-role 하한, 설정값); resend-window boundary-batch memory사본은 in-flight budget에 불포함 | 28 §5.3 (453-458행) | |
| R217 | Application Job Queue 공유permit 용량≠relocation전용 용량; 일반staging ingress는 공유예약 사용, durable handoff 직후 반환; CAS+lifecycle 통과한 runnable turn만 live permit 보유 | 28 §5.3 (460-464행) | |
| R218 | CAS결과표(성공=target소유·무rollback / 조건불일치=무변경,현재owner유지,target제거,relay-ready accepted후 source 미재개 / retryable Store실패=Restore deadline까지 retry / 무응답=원래key·version 재읽기 또는 retry / 다른valid owner·generation=즉시stale종료 / deadline초과=location_update_failed) | 28 §6 (472-479행) | |
| R219 | Session owner가 검증하는 4값(physical Session identity+SessionRid, binding generation, bound ActorId+ObjectGeneration, relocation identity), Store 재조회 안함, one-way route update, duplicate no-op — 전체 | 28 §7 전체(484-505행) | **세션문서 §8 링크(§4 S1)** |
| R220 | Actor·Spot-type adapter표(Cross-node Actor Join / Entry Spot host relocation / PerActor authority / SpotWide / Instance Spot 각행의 추가CAS field+callback 차이) | 28 §8 (513-521행), **52 §9 adapter표와 동일 5종**(52 298-307행, "queue-merge·CAS·timeout·Session책임은 adapter마다 재구현 안함" 단서 포함) | |
| R221 | 실패·timeout표(§9) 15개 시나리오행(owner유지·결과열) | 28 §9 (527-540행), **44 §4 실패3행 요약과 동일내용**(44 119-123행) | |
| R222 | 재조정(reconciliation) 원칙 — cutover submit결과 불명시 Message Follow duration내에 한번 Location Store authority 재확인(target commit확인시 target route채택+backlog forward-drain / 여전히source면 local dispatch복원+backlog replay / 불확정·읽기불가면 대기중request를 explicit Unavailable로 실패, unit은 unavailable유지, 다음sweep에 재시도) | 28 §9 (540행) | |
| R223 | Relay-ready-accepted후 source dispatch는 cutover submit결과와 무관하게 재개 안함; 이후 target CAS실패 시 target은 준비된unit 제거, Session은 자체timeout으로 자가정리 | 28 §9 (542-545행) | |
| R224 | Reconciliation원칙 — source dispatch는 Store증거(여전히 source가 owner)로만 재개, deadline자체는 재개 안시킴(대기만 경계) | 28 §9 (547-550행) | |
| R225 | Cutover-wait fallback은 TCP재전송 대체 아님(late relay와 new target message간 순서보장 없음); resend(§4.4)는 이경로 진입빈도만 줄임 | 28 §9 (552-555행) | |
| R226 | Store outage가 Restore-deadline까지 지속시 Session은 자체seal timeout으로 별도종료 가능; 이후 Store복구돼도 새Session connection이 옛binding을 복원하지 않음(정상 위치검증·생성·복구절차 진행) | 28 §9 (557-560행) | |
| R227 | Cleanup실패는 target ownership을 source로 되돌릴 근거 아님 | 28 §9 (584-585행) | |
| R228 | Message Follow는 원래operation identity·ObjectGeneration·payload·source routing id·reply route 보존(Store재읽기 없음, source-side handler 실행 없음); **MessageFollowDuration 기본30초·최대8 hop·순환=Unavailable·generation불일치=InvalidOperation은 21 §6.3이 canonical 소유(§4 S2), 04는 링크만** | 28 §10 (568-571행), 44 §3 (71-87행) | |
| R229 | Followed-operation의 end-to-end deadline은 client관리(hop마다 절대값 전파 안함, 각relay hop이 자기 local relay-window 대기 재설정) | 28 §10 (573-576행) | |
| R230 | 늦은cutover·Session route update는 Message Follow기간 연장 안함; 이른Session route전환이 이미 옛주소로 보낸 server message를 즉시 폐기시키지 않음 | 28 §10 (578-580행) | |
| R231 | 보장/비보장표 8행(§11) — cross-connection 전역순서 없음, crash-window exactly-once 없음(같은-process retry시 app callback·side-effect 중복실행 가능) 등 | 28 §11 (589-598행) | |
| R232 | **금지기법 11개**(mailbox 전체drain 대기 / message별ACK relay protocol / numeric high-water queue reconciliation / TCP중복되는 durable delivery journal / relocation전용 record·byte·concurrent-unit capacity gate / dispatch open전 whole-backlog permit 사전예약 / source·Session이 직접수행하는 Location Store owner변경 / ACK-timeout기반 투기적 source rollback / global cross-TCP-connection message순서 / **target payload 부분조립 자가복구 금지**(checksum·length 불일치는 항상 명시적 relocationFailed로 끝냄, target은 부분조립을 스스로 복구 안함) / **exact RelocationId+targetAttemptGeneration+coordinator fence+carrying connection이 아닌 도착순서·최신성으로 prepare·chunk·CAS를 relocation에 귀속시키는 것 금지** / **같은target queue에 대해 두개의 Actor-Join prewarm-prepare attempt를 동시유지하는 것 금지(새identity 도착시 기존prepare abort, 최신시도가 항상 승리)**) | 52 §8 (273-289행) — 마지막3개는 28에 없던 net-new(§3.1) | |
| R233 | 기존 non-relocation 자원제한(runtime memory·frame크기·Store page·payload)은 그대로 적용, relocation전용 state·새public setting으로 중복 안함 | 52 §8 (291-292행) | |
| R234 | **구현결정**(44 §5): 이동하나의 relocation은 정확히 하나의 상태-전이규칙이 소유(component별 독립진화 state로 안쪼갬 — 실패시 cleanup 소유자를 숨기고 §4 비대칭처리를 branch마다 재구현하게 되므로) | 44 §5 (159-167행) | |
| R235 | **결정**(52 §1): pre-owner-commit=source 단독owner, post=target 단독owner, 복구경계=relay-ready accepted(owner-commit 자체 아님); phase 저장표현(enum·record)·직렬화primitive(lock·actor-loop·executor)는 언어재량이나 전이순서·허용된 역전은 재량 아님 | 52 §1 (45-52행) | |
| R236 | 구현 및 contract test 검증요구 통합(28 §12 30항목+44 §6 10항목+52 §10 20항목, 중복 제거 후 하나로) | 28 §12 (600-651행), 44 §6 (171-181행), 52 §10 (309-333행) | |
| R237 | Message Follow forwarding volume에 relocation전용 상한 없음(21 §6.3 링크로 충분, §4 S2) | 44 §3 (78행) | |

### 5.5 30-host-relocation-flow

§8의 unit mechanics(temporary queue·checksum·cutover·CAS)는 04(28)의 R182~R209와 동일 메커니즘의
재서술이다(§4 S3) — 아래 표에서 "동일" 표시된 행은 04의 해당 R#을 그대로 가리키도록 재작성하고
본문 문장은 남기지 않는다.

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R238 | Relocate는 graceful handoff만 지원(source·target·Location Store가 끝날때까지 실행); process종료뒤 자동복구는 범위밖 | 30 §1.1 (38-43행) | |
| R239 | Owner 이중화방지 원칙(Store결과 불명확시 추측없이 재조회, 실제owner 확인전 admission재개·dispatch시작 금지); Application은 component만 골라 종료순서를 직접조립 안함 | 30 §1.1 (45-54행) | |
| R240 | Mode값 PlannedMaintenance=0·RollingUpdate=1; PlannedMaintenance는 TargetApplicationVersion 미지정(지정시 argument error); RollingUpdate는 source보다 큰버전 필수(없거나 이하면 argument error); 잘못된조합은 state·admission변경전 거부 | 30 §2.1 (87-95행) | |
| R241 | Deadline 기본30초(생략시 사용), 명시값은 0보다 커야함 | 30 §2.1 (98-99행) | |
| R242 | ZLinkFrameworkRelocationResult엔 mode·target version 항상포함; Relocate가 Blocked로 끝나면 caller는 재시도 또는 continuity없이 Shutdown 가능 | 30 §2.2 (179-182행) | |
| R243 | FrameworkRuntimeState값(Preparing=0·Serving=1·Relocating=2·Relocated=3·Draining=4·Stopped=5·Error=6); IsReady는 Serving에서만 true; component별 Drain·AwaitDrained·Stop이나 일부Mesh만 대상하는 public operation은 없음 | 30 §3 (192-206행) | |
| R244 | State전이규칙(mermaid) — Relocating→Serving은 "Blocked뒤 source처리 복원"만 가능; relay-ready accepted전 명시실패만 tentative작업 정리·source복원 가능, 경계지난unit은 rollback없음(Serving복귀가 전체unit rollback을 뜻하지 않음) | 30 §3 (208-229행) | |
| R245 | Relocation Outcome값(Relocated=0(reason None) · Blocked=1(reason 10종: TargetUnavailable·StoreUnavailable·RelocationDisabled·StateIncompatible·DeadlineExceeded·RelocationFailed·RuntimeNotReady·ManualTopologyUnsupported·ShutdownRequested·OperationInProgress)); wire값매핑(0/1, reason0~10); 정의없는조합은 protocol오류 | 30 §3 (231-243행) | |
| R246 | Shutdown outcome(Stopped=0·ForceStopped=1, reason None=0·DeadlineExceeded=1·TeardownFailed=2); ForceStopped는 별도host state 아님(host state는 Stopped); Relocation실패는 termination reason에 안섞음 | 30 §3 (245-248행) | |
| R247 | Preflight는 target 새작업차단·수용공간 사전확보 안함; 5개검사항목(동시진행작업·Local workload·Store·Unit호환성·Topology) | 30 §4 (253-267행) | |
| R248 | Manual RouteMesh peer·ClientServer client endpoint·fanout subscriber endpoint·미게시 manual fanout publisher가 하나라도 등록돼있으면 Blocked/ManualTopologyUnsupported(현재연결여부 아니라 registration기준); Runtime확인범위는 local registration만 | 30 §4 (269-277행) | |
| R249 | Target 좁히기 5단계순서(version일치 → source아니고 Serving → factory·stable-type·policy·adapter 호환 → 수용공간+maintenance wave값이 다른node → 같은시점 descriptor목록·Core peer table에서 RID·lifecycle generation 동일&peer가 Admitted+Ready); Version은 수용공간·weight보다 먼저확인 | 30 §5 (283-292행) | |
| R250 | Descriptor게시·connect intent만으로 target ready 판단안함; snapshot이 비었거나 source자신만 포함하거나 모든remote peer가 draining이면 target없음 | 30 §5 (296-300행) | |
| R251 | 이전할unit 없으면 target탐색없이 Relocated/None으로 종료(host state전이·admission종료는 동일); target없고 unit있으면 deadline까지 수렴대기(여러Mesh는 전부조건필요), deadline초과시 tentative coordination정리후 Blocked/TargetUnavailable | 30 §5.1 (304-313행) | |
| R252 | Relocated는 target CAS완료확인이 아니라 "모든unit의 cutover submit시도가 terminal도달"이라는 source측 결과(descriptor·connection·listener·infra resource는 이때 유지) | 30 §5.1 (339-341행) | |
| R253 | Automatic ClientServer client·fanout subscriber는 replacement descriptor로 새connection(source는 selection제외); accepted work·barrier 남은 기존connection은 descriptor변화만으로 즉시 안닫음 | 30 §5.1 (345-347행) | |
| R254 | Concurrent Relocate 매트릭스(같은mode+target version=최초operation deadline공유 / 다른mode·version=대기없이 Blocked·OperationInProgress / Blocked뒤 재호출=저장안하고 처음부터 재검사 / Relocated에서 재호출=최초결과 반환); Concurrent Shutdown=같은operation 공유(terminal결과 저장); Stopped에서 재Shutdown=저장된결과 또는 새작업없이 Stopped/None | 30 §6 (353-358행) | |
| R255 | Caller cancellation은 해당waiter만 종료(shared operation 불취소); Preparing·Error·Stopped에서 Relocate는 admission불변+Blocked/RuntimeNotReady; Shutdown은 Preparing에서 startup중단, Error에서 bounded cleanup시작 | 30 §6 (359-363행) | |
| R256 | Relocation unit 4종(SpotWide User Spot aggregate·Actor(Entry·PerActor)·PerActor Spot authority전환·Instance Spot); Entry Spot 자체는 이전 안함(Actor만 unit) | 30 §7 (373-380행) | |
| R257 | Inventory는 Relocating전환 직전 active state 한번 읽어 확정(확정뒤 새stateful placement 안받음); 같은Actor를 Spot unit과 standalone unit에 중복 안넣음 | 30 §7 (384-389행) | |
| R258 | Batch순서 3단계(Batch1=PerActor Spot shell·queue authority(조건: inventory+preflight완료, current turn종료) / Batch2=Entry·PerActor·standalone Actor(각 current turn종료, PerActor는 shell전환완료) / Batch3=SpotWide aggregate·Instance Spot(aggregate전체 current turn+safe point완료)); 앞batch terminal도달후 다음시작, 실패해도 이미시작한unit은 안전terminal까지 처리 | 30 §7 (394-404행) | |
| R259 | 동시실행unit수·participant수·relay record수에 상한없음(in-flight payload예산이 조절, 계산·대기규칙은 04 §5.3 소유); 예산초과시 source admission seal 적용전 대기(대기중 Actor·Spot은 정상처리 계속); limit도달만으로 시작된relocation 실패 안시킴 | 30 §7 (415-424행) | |
| R260 | SpotWide는 current turn끝나면 spot+그시점 member전체 한unit; PerActor·Entry는 Actor별 완료순; PerActor Spot lane은 authority전환때만 잠깐막고 member전체를 안기다림; ApplicationSignaled SpotWide는 target준비후 RelocationReady().Defer() turn경계 사용(준비된relocation없으면 다음turn Continued callback, accepted전 취소시 source복원후 Continued, 경계뒤엔 취소·실패로 source복원 안함, owner commit뒤 target queue에서 Relocated를 첫turn으로 호출) | 30 §7 (441-451행) | |
| R261 | 측정unit(Entry Spot Actor=Actor1개 / PerActor User Spot=Spot direct admission1개+Actor각각 / SpotWide=aggregate1개 / Instance Spot=Spot1개); 측정시작=source admission seal 적용시점(target준비·turn대기시간 제외, seal후 Capture~cutover terminal까지 포함) | 30 §7.1 (455-468행) | |
| R262 | 시점정의 S0~S4(S0=Source admission seal / S1=one-way cutover submit terminal / S2=Target Location Store CAS확인 / S3=Target dispatch개방 / S4=Message Follow route제거가능시점); 지표(Source정지시간=S0→S1 / Target재개시간=S2→S3 / Route수렴시간=S1→S4); 서로다른node시각을 직접빼는 지표는 안만듦(cross-node구간은 26 message flow tracing으로만 관찰) | 30 §7.1 (470-489행) | |
| R263 | **각unit 기본목표 1초**(timeout도 correctness조건도 아님, 초과해도 취소·rollback 안함, warning+`zlink.relocation.interruption` histogram 기록); Target은 처리시작 ACK 안보냄 | 30 §7.1 (495-500행) | |
| R264 | Host operation deadline종료시 새unit 시작안함; 이미시작한 unit중 relay-ready reply전 명시실패한 unit만 안전abort(reply결과 불확정이면 target cutover대기 fallback가능성 있어 source dispatch 재개안함); cutover전송 시도한 unit도 rollback안함 | 30 §7.1 (502-508행) | |
| R265 | §8.2 공통순서 9단계(temporary queue설치→chunk전송→checksum확인→ingress hold relay→cutover→CAS→queue병합→dispatch개방→session route)는 **04 §4의 R182~R209와 동일 메커니즘** | 30 §8.2 (532-656행) | **04 §4 링크(§4 S3)** |
| R266 | RelocationCutoverWaitTimeout(cutover대기) 기본1,000ms — 30 자신이 소유문서를 06-framework-api로 명시(R197과 동일값) | 30 §8 (572-573행) | |
| R267 | DisableRelocation·RecreateOnRelocation·PreserveStateWith policy별 target factory 동작표 | 30 §8 (651-655행) | |
| R268 | Relocation unit하나의 temporary queue엔 record수·저장크기 상한없음(같은object에 추가temp queue 안만듦); SessionRelocationSealTimeout 기본3,000ms(§4 S1, 세션문서 §8 소유) | 30 §8 (590-593, 602-603행) | |
| R269 | §8.5 SpotWide 이동ID는 0아닌 128-bit값; User Spot소속 Actor총수엔 상한없음(Location Store 페이지단위 한페이지 최대1,024개·최대1 MiB, 예: 2,500개면 최소3페이지); 전체Actor수·페이지내용이 처음저장 목록과 일치할때만 한번에 CAS(중간충돌시 일부만 안바꿈) | 30 §8.5 (790-806행) | |
| R270 | Participant 하나 실패하면 temporary queue의 어느작업도 실행안하고 group전체 폐기 | 30 §8 (796-797행) | |
| R271 | §8.7 Entry Spot·PerActor Actor relocation은 OnActorJoin·OnJoinedActor·OnLeaveActor 호출안함; SpotWide도 동일(ApplicationSignaled면 OnRelocationReadyCompleted(Relocated)만); User·Instance Spot source instance는 위치변경후 OnClosing(RelocationOut) 호출(Entry Spot instance는 closing callback없음, Instance Spot은 Actor lifecycle callback 자체 없음) | 30 §8.7 (898-910행) | |
| R272 | §8.8 Target CAS 끝내실패시 target object·queue제거, Session은 자체seal timeout으로 정리, source Message Follow도 정해진기간에 종료 | 30 §8.8 (925-930행) | |
| R273 | 새작업 차단뒤 도착message는 source가 record수·크기 상한없이 보관(owner변경성공시 operation identity·ObjectGeneration 유지전달, accepted전 취소는 순서대로 source queue복원, 그뒤는 복원안함) | 30 §9 (955행) | |
| R274 | SpotWide·Instance Spot timer는 runtime handle·continuation을 이전 안함(logical registration·다음실행시각·pending tick만 이전, target이 queue순서 맞춰 자동복원); application이 timer 중복capture·재등록 안함 | 30 §9 (956행) | |
| R275 | Entry·PerActor Actor timer는 Actor queue와 함께 이전; Spot-level application timer는 이전 안함(유지필요 schedule은 application 외부state에서 관리) | 30 §9 (957행) | |
| R276 | Session행(958행)은 **04 §7 · 세션문서 §8이 소유**(§4 S1·S14) | 30 §9 (958행) | **04 §7 링크** |
| R277 | Instance Spot Close와 relocation은 같은authority commit에서 순서결정(Closing먼저면 close완료후 이전안함, relocation먼저면 늦은Close는 moving결과로 자동재제출 안함) | 30 §9 (967-969행) | |
| R278 | Relocated/None조건=모든unit이 source dispatch에서 분리+relay-ready reply를 보낸 각target에 cutover submit이 terminal도달(target Location Store CAS완료확인 아님, descriptor·lease·listener·peer connection·raw transport는 이때 정리안함) | 30 §10 (973-977행) | |
| R279 | 완료지점 5단계표(Restore+relay-ready reply / one-way cutover submit terminal / Relocated·None reply / Location Store CAS성공 / Session route update적용) — **마지막행은 세션문서 §8 소유(§4 S1)** | 30 §10 (979-985행) | |
| R280 | Target은 cutover reply나 Session route update reply를 안보냄(source는 ACK journal·high-water 안만듦) | 30 §10 (987-988행) | |
| R281 | 재전송창=RelocationCutoverWaitTimeout과 같은시간(창안 재연결시 source가 batch+cutover 재전송, target은 부분수신구간 폐기하고 전체교체); 사본은 source memory보관, 창끝나면 정확히한번 정리후 재전송 안함; 재전송창은 완료지점을 안바꿈(host는 최초cutover submit terminal에서 Relocated전환) | 30 §10 (990-999행) | |
| R282 | DeadlineExceeded 원인별 결과매핑 6항목(target후보 미준비→Blocked/TargetUnavailable / Store실패(accept전)→record정리+Blocked/StoreUnavailable / DisableRelocation잔존→Blocked/RelocationDisabled / state adapter비호환→Blocked/StateIncompatible / deadline으로 callback취소(accept전)→Blocked/DeadlineExceeded / target이 accept전 Restore명시거부→source복원+Blocked/RelocationFailed) | 30 §10 (1007-1014행) | |
| R283 | Checksum불일치시 target은 부분조립 복원 안함(accept전 명시실패로 응답, source는 memory보관 payload로 queue복원); SpotWide 이동대상목록 확인값이 처음저장 목록과 다르면 DataLost(재시도해도 복구불가, 이전목록 추측·되돌리기 안함) | 30 §10 (1027-1031행) | |
| R284 | 일부MeshNode의 Relocating descriptor 기록확인 실패시 시도한 모든descriptor를 Serving으로 되돌림(모두확인돼야 Blocked/StoreUnavailable, 하나라도 불가하면 정해진 최대시간 정리후 ForceStopped/TeardownFailed) | 30 §10 (1033-1036행) | |
| R285 | Shutdown은 target·policy·capacity·Relocation Store 부재로 차단안됨; 6단계순서(Draining전환+신규admission·relocation차단 → Draining descriptor게시 → 이미수락한작업 deadline까지처리 → 새object relocation안시작+HostShutdown closing context전달(Actor별 closing callback은 호출안함) → local scope·owner record·descriptor·listener·transport 순서대로정리 → deadline내 Stopped/None, 아니면 ForceStopped) | 30 §11 (1040-1057행) | |
| R286 | 경쟁처리표(Shutdown admission seal 먼저확정=target수용공간 반환+대기Relocate를 Blocked/ShutdownRequested로 종료 / Relocating publication 먼저확정=현재unit만 terminal까지 확정, 나머지 시작안함, waiter는 Blocked/ShutdownRequested); Relocated의 Shutdown은 accepted work+infra만 정리(Serving에서 바로호출시 object이전 안함) | 30 §11 (1065-1071행) | |
| R287 | MessageFollowDuration전체를 쓰려면 그기간 끝난뒤 Shutdown호출(먼저호출하면 남은route+재전송사본도 함께정리); SafeToShutdown 게시조건=모든unit이 S4도달+각unit 재전송창종료(둘다 source-local사건) | 30 §11 (1075-1084행) | |
| R288 | Draining정리 4단계순서(Spot closing callback+local scope정리 → current authority가진 source만 owner·이동대상record 변경·제거 → descriptor·owner lease release → peer connection·listener·executor·binding transport 닫기) | 30 §11 (1099-1105행) | |
| R289 | Callback exception→ForceStopped/TeardownFailed; deadline만료→ForceStopped/DeadlineExceeded; Hardware failure·SIGKILL에서 callback보장 안함; 종료중 relocation·cleanup을 다른runtime이 자동이어받는것 보장 안함 | 30 §11 (1110-1113행) | |
| R290 | State별admission 매트릭스(9개공개기능×3state: Relocating·Relocated·Draining); 이미수락한 request는 reply·error·timeout·shutdown 중 하나로 한번만 종료 | 30 §12 (1126-1139행) | |
| R291 | 관측event이름(zlink.runtime.host.relocation_changed·zlink.runtime.host.termination_changed); Terminal event는 observer overflow로 안잃음; Version을 metric label로 안넣음; Actor ID·Spot ID·node RID·endpoint·session ID·relocation ID를 metric label에 안넣음; Telemetry provider failure는 operation진행을 안막음 | 30 §13 (1146-1167행) | |

### 5.6 31-failure-failover-policy

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R292 | Application이 별도 FailoverPolicy를 선택하는 public API 없음(Framework가 operation종류·장애시점에 따라 고정규칙 적용); "처리대상 선택"만으로 operation실행 확정으로 안봄(accept불분명·이미accept뒤엔 중복방지위해 다른대상에 자동재제출 안함) | 31 §1 (16-23행) | |
| R293 | Failover정의=장애처리 대상을 다른대상으로 바꾸고 작업계속; Reconnect·계획된Relocate·재조회는 failover와 구분 | 31 §1 (25-28행) | |
| R294 | 공통판단기준 5단계순서(target identity 직접지정여부 → transport·target queue accept여부 → Location Store owner+generation 유효성 → stateful relocation이면 relay-ready accepted전·후·commit후 구분 → 계속불가하면 한번의terminal결과로 종료); 경계별처리 매트릭스 5행 | 31 §2 (32-54행) | |
| R295 | Application이 idempotency key·상태확인으로 중복영향 방지해야함; Framework는 실행여부 불분명한 앞선operation을 "실행안됨"으로 안되돌림 | 31 §2 (56-59행) | |
| R296 | Select-one 후보(RouteMesh Channel=Ready+weight>0 / ClientServer Channel=Ready만); Non-blocking submit이 capacity부족으로 미수락시 transport queue가 accept할때까지 다른eligible server 선택가능(accept뒤엔 reply없거나 connection끊겨도 다른server 재실행 안함) | 31 §3.1 (66-72행) | |
| R297 | Node direct는 재선택규칙 미사용(지정node없거나 connection미준비시 NotFound 또는 Unavailable) | 31 §3.1 (74-76행) | |
| R298 | Orderly close·transport오류는 즉시반영; 응답없는 half-open connection은 liveness deadline안에 not-ready전환(한peer장애가 다른ready peer·local owner처리를 중단시키거나 host전체를 Error로 안바꿈); Reconnect시 handshake+identity 재확인(이전connection ID·reply route·Session binding·ready상태 재사용 안함); connection loss전 accept여부 불명이면 다른peer에 제출 안함 | 31 §3.2 (81-88행) | |
| R299 | 일반Actor·Spot message는 global logical ID만 target(ObjectGeneration은 target일치조건에서 제외 — 같은ID로 재생성된것과 owner를 잃은것이 다른결과: 전자는 새incarnation처리, 후자는 Unavailable) | 31 §4.1 (100-104행) | |
| R300 | Cache만료·owner lease무효시 **다음 새operation**이 현재owner 재조회(실패한operation 자체는 자동재제출 안함) | 31 §4.2 (108-110행) | |
| R301 | **Message Follow는 failover가 아니다** — MessageFollowDuration 기본30초, 0이면 미사용; 이미commit된 이동경로를 따를뿐 owner process장애뒤 새owner를 선택하지 않으므로 | 31 §4.2 (113-116행), **21 §6.3·44 §3과 같은값(§4 S2, 21이 canonical 소유)** | |
| R302 | 현재Ready owner process종료시 Framework는 다른node에 자동복원 안함(Location Store owner를 임의로 안바꿈, 같은global ID의 새incarnation 안만듦); Instance Spot에도 동일적용(종류만으로 lease만료뒤 authority release나 cold activation전환 안함) | 31 §4.2 (119-123행) | |
| R303 | 생성경쟁시 Creating record를 먼저확보한 target하나만 factory실행; 생성중 process종료시 다음operation이 같은object ID+generation의 생성record 재확인(계속 또는 취소가능, factory는 같은입력으로 재호출가능); 생성recovery는 Ready공개전 recovery(이미실행중인 object의 owner장애복구=failover 아님) | 31 §4.3 (127-133행) | |
| R304 | Instance Spot cold activation 6상태×처리매트릭스(Missing·Creating·Ready미복원·Ready lease유효·Ready owner종료·lease무효·Close완료·Relocate진행·완료); Ready owner장애시 authority자동release 안함, 다른node에 새incarnation 안만듦, Unavailable로 종료 | 31 §4.4 (149-156행) | |
| R305 | "process종료뒤 lease만료되면 다음message가 다른node에서 재활성화" 동작은 **현재계약에 없음**(그런동작을 원하면 별도failover계약 필요: authority release조건·state/operation복구방법·이전owner fence방법 3가지 미정) | 31 §4.4 (158-161행) | |
| R306 | 최초생성recovery 정보는 Instance Spot 최초생성에만 사용(Actor·User Spot·이미Ready인 Instance Spot·host relocation엔 적용 안함) | 31 §4.4 (163-164행) | |
| R307 | Relocate는 장애host를 대신할 owner를 찾는 operation이 **아니다**(graceful handoff만 지원); 실패시점별 처리 6행표(relay-ready accepted전 명시실패=target폐기+source유지+다른target 자동선택안함 / accepted후~commit전=submit결과무관 source복원안함, target은 cutover수신 또는 1,000ms fallback으로 계속 / Store결과미수신=추측없이 재조회 / commit후 같은target실행중=rollback안함, deadline안 재시도가능 / commit후 target process종료=Location Store owner유지하되 object Unavailable, 다른runtime이 안이어받음 / source·target process가 operation중 종료=다른target선택·재시작후재개·source rollback모두 안함) | 31 §5 (170-182행) | |
| R308 | Relay-ready accepted전 source유지는 failover가 아니라 "비가역경계전 operation의 취소"(용어구분) | 31 §5 (184-186행) | |
| R309 | Actor 계획된relocation시 Session physical STREAM connection유지(Target runtime이 Session owner에 binding route+ActorRef snapshot 갱신message); ObjectGeneration유지된 relocation에만 적용, Application은 재bind 불필요 | 31 §6 (193-197행) | |
| R310 | Actor제거 또는 owner장애뒤 같은ActorId 새incarnation시 이전binding은 종료상태유지(Session relay는 current binding token 필요→Application이 새ActorRef bind필요); 이전Session의 늦은relay·unbind·disconnect는 새binding에 적용 안함 | 31 §6 (199-202행) | |
| R311 | Session owner process종료시 physical connection·identity·binding을 다른process로 이전 안함; client reconnect는 새Session생성(재인증+재bind필요); 이전connection의 reply·binding update는 새Session에 적용 안함 | 31 §6 (204-206행) | |
| R312 | Location Store변경 결과미수신시 성공·실패 추측 안함(같은key+처음사용 StoreVersion으로 재조회, 적용확인후 필요시에만 재시도) | 31 §7 (212-214행) | |
| R313 | **StoreFailureGrace**동안 마지막완전읽은 descriptor목록 유지(기존connection liveness확인 계속, 새outbound connection 안만듦); Grace는 owner lease·relocation deadline을 **연장 안함**(owner자격종료되면 새message·timer처리·state변경 중단); Store복구되면 owner·descriptor전체 재확인후 필요한 connection변경만 적용 | 31 §7 (217-221행) | |
| R314 | Framework는 ErrorKind를 반환하지만 재시도여부는 제공안함(timeout·connection loss시 remote handler실행여부를 알수없기 때문); Framework의 수락전 재선택·Store재확인은 "같은operation의 내부처리"이며 application이 실패후 시작하는 새operation과 구분 | 31 §8 (228-234행) | |
| R315 | 이문서는 공개장애동작의 소유문서(internals문서 45·47·49·51은 이장의 오류의미·failover범위를 재정의 안함) | 31 §10 (267-268행), **원문 "정본"→재작성시 "이 문서가 소유하는 최종 규칙"으로 정정(§4 S12)** | |

## 6. 링크·코드·site 영향

| 대상 | 처리 |
|---|---|
| 스펙 내부 링크 (21·22·23·28·30·31을 참조하는 문서 다수, 44·52는 각각 15·11개 파일에서만 참조) | 새 경로·새 절 anchor로 치환. 44·52의 참조 15+11개는 병합으로 `04-relocation-flow`의 해당 절 anchor로 통합 치환 |
| 외부 참조(21: 90파일·22: 41·23: 26·28: 72·44: 15·52: 11·30: 92·31: 50) | 같은 치환표로 sed. 언어별 guide는 `generate_language_guides.py`가 공통 guide에서 생성하므로 공통 guide만 고치고 재생성 |
| `testdata/location/redis/*.json` 5개(`actor-location-v2`, `authority-store-v1`, `client-server-server-descriptor-v1`, `fanout-publisher-descriptor-v1`, `mesh-node-descriptor-v1`) | `"notice"` 필드의 `22-location-store-redis.ko.md` 경로를 `05-location-relocation/02-location-store-redis.ko.md`로 갱신 |
| `authority-store-v3.json` | notice 필드에 명시 경로 없음(grep 결과 미검출) — 내용 확인 후 필요 시 같은 갱신 적용 |
| Redis provider 구현체의 파일경로 주석(dotnet·java·cpp·node 약 40곳) | 코드 자체는 이 캠페인에서 건드리지 않음(§1). 이동 단계에서 주석에 남은 옛 spec 경로 문자열이 있으면 별도 후속 작업으로 갱신 여부 판단(코드 변경이므로 이 캠페인 범위 밖) |
| cpp layout contract test | 해당 없음 — 8개 문서 중 어느 것도 needle 검색 대상이 아님(§1 확인) |
| `target-readme.ko.md` 번호 오타(162-177행, `04. relocation-flow`와 `04. host-relocation-flow` 중복) | 이 주제 완료 시 `04-relocation-flow` / `05-host-relocation-flow` / `06-failure-failover-policy`로 정정 |
| mkdocs nav | "Location and relocation" 그룹 → `05-location-relocation/README`, `01-location-runtime`, `02-location-store-redis`, `03-relocation-store-redis`, `04-relocation-flow`, `05-host-relocation-flow`, `06-failure-failover-policy` |
| redirect | 캠페인 말미 site 작업에서 `21-…`→`05-location-relocation/01-…`, `22-…`→`…/02-…`, `23-…`→`…/03-…`, `28-…`/`44-…`/`52-…`→`…/04-relocation-flow`(세 옛 경로 모두 같은 새 경로로), `30-…`→`…/05-…`, `31-…`→`…/06-…` |
| 검증 | `check_doc_links.py`, `mkdocs build --strict`, `git diff --check` |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)에 다음을 준다.

- 입력: 새 `01-location-runtime` ~ `06-failure-failover-policy` 6개 문서와 §5 대장(새 위치 열 채운 것)
- 과제: 대장의 행마다 해당언어 구현에서 **일치 / 불일치 / 스펙 미정 / 판단 불가** 중 하나와 근거(파일:줄)
- 특히 주의 깊게 볼 항목 — 병합·경계정리로 문장이 크게 바뀌는 R197(1,000ms 값이 04/30 양쪽에 있던 것이 04로 단일화), R228(Message Follow 값이 21로 단일화), R210·R219·R276·R279(Session owner 서술이 세션 문서로 이관), R232(52의 dual-prewarm-prepare 금지 규칙이 실제 canonical multi-attempt 정책과 일치하는지)
- 금지: 스펙 수정, 코드 수정. 판정은 하지 않고 사실만 보고
- 출력 양식: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **문서 오류** → 수정.
옛 문서 때부터 달랐거나 언어끼리 다르면 **spec gap** → [spec-gap 대장](../../spec-gap.ko.md) 등록,
문서는 계약 의도대로 유지. G7(52 abort 순서)은 이미 판정 완료 상태이므로 이 주제의 판정에서는
"자동 — 이미 반영"으로만 확인한다.

## 8. 작업 순서

1. `05-location-relocation/README.ko.md` 초안(§2 질문표 기준) — 문서 소속·중복 판정의 최종확인
2. `01-location-runtime` 재작성(ko) → §5.1 대장 R1~R113 새 위치 채움(§4 S4·S6·S7 구조정리 포함)
3. `02-location-store-redis` 재작성(ko) → R114~R144(§4 S5 소제목 분리 포함)
4. `03-relocation-store-redis` 재작성(ko) → R145~R176
5. `04-relocation-flow` 재작성(ko, 28+44+52 병합) → R177~R237, §4.1·§4.2(G7)의 세션 서술 이관·abort 순서 정정 포함, ~680~720줄 목표
6. `05-host-relocation-flow` 재작성(ko) → R238~R291, §8 대폭축소(§4 S3)
7. `06-failure-failover-policy` 재작성(ko) → R292~R315(§4 S12·S13 용어·링크 정리만)
8. 등가성 대조 — 대장 빈 행 0, 추가 보장 0(315행 전체 grep 확인)
9. en 짝 작성
10. 링크치환·guide재생성·nav·`testdata/location/redis/*.json` notice 필드 갱신 → 검증 3종 그린
11. 구현대조(§7) → 판정·기록, G7 자동확인
12. `target-readme.ko.md` 번호 오타 정정(§6)
13. 한 커밋(문서 이동+내용) + spec-gap 대장 갱신

## spec-gap 후보

이 주제에서 새로 발견한 **실제 스펙 결함**은 없다. 조사 과정에서 나온 항목은 모두 문서 구조
문제(§4 S1~S17)로, 재작성으로 해소되며 계약 자체를 바꾸지 않는다.

- **G7 재확인**: 52 §5의 abort 순서 역전은 이미 spec-gap 대장에 등록돼 있다(§4.2에서 재확인).
  새 행을 만들지 않고, 병합 시 세션 문서·28·44와 같은 순서로 정정한다.
- 30 §8의 앵커-헤더 불일치(365행 vs 367행)와 31 §10의 "정본" 용어는 문서 버그·어휘 문제이지
  계약 결함이 아니므로 spec-gap에 올리지 않고 재작성에서 바로 고친다(§4 S11·S12).
- 30 §7.1의 interruption budget 1초(관측용 warning 임계값)와 04(28)의 `RelocationCutoverWaitTimeout`
  1,000ms(프로토콜 fallback 시한)는 값이 같지만 서로 다른 개념이다 — 모순이 아니라 재작성 시
  두 값을 혼동하지 않도록 각 문서에서 "이 값은 X를 재는 것이지 Y가 아니다"라고 한 번씩 명시하면
  충분하다(구조 문제로 처리, gap 아님).
