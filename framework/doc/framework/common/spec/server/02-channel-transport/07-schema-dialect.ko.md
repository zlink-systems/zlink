---
title: "Service wire schema dialect"
---

# Service wire schema dialect

[Channel·Transport 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. Service wire protocol](06-wire-protocol.ko.md)

> **이 장이 답하는 것** — `framework/runtime/protocol/service-wire-v1.schema.json`을 어떻게 읽는가.
> 최상위 키 하나하나가 무엇을 선언하고, `types[].kind` 열 가지가 byte를 어떻게 배치하며,
> `$ref`·`$bound`·`when`·`constraints` 같은 keyword를 encoder와 decoder가 어느 시점에 어떻게
> 평가하는가.
>
> **계약 소유** — 값은 schema가 소유하고, 읽는 법은 이 장이 소유한다. 이 장은 command 표, field
> 목록, 숫자 상한, enum 값을 적지 않는다 — 그 값이 필요하면 schema를 연다. 반대로 schema에 새
> 최상위 키, 새 `kind`, 새 keyword가 들어오면 그 변경은 dialect 변경이므로 같은 변경에서 이 장을
> 갱신한다. 이 장이 설명하지 않는 키나 keyword가 schema에 있으면 schema가 아니라 이 장의 결함이다.
>
> **함께 보는 계약** — [06. Service wire protocol](06-wire-protocol.ko.md)(frame 구성, command
> 목록, decode 상한과 검증 요구) · [Message model §5](../00-foundation/05-message-model.ko.md#5-framework-json-v1-typed-payload-profile)(typed
> payload JSON) · 도구 사용법은 [`framework/runtime/protocol/README.ko.md`](../../../../../../runtime/protocol/README.ko.md)

| 절 | 다루는 내용 |
|---|---|
| [1. Dialect와 소비자](#1-dialect와-소비자) | dialect 식별, schema를 읽는 세 소비자, 문장형 문자열 값의 지위 |
| [2. 최상위 키](#2-최상위-키) | 28개 키 — dialect 식별자 둘과 네 묶음(protocol 상수, layout 선언, semantic 선언, profile 선언) |
| [3. 모든 layout에 공통인 규칙](#3-모든-layout에-공통인-규칙) | byte order, 정수 encoding, `$ref`·`$bound`, 크기 상한과 allocation 순서, 실패의 종류 |
| [4. `types[].kind` 열 가지](#4-typeskind-열-가지) | kind별 선언 키, byte 배치, decode 실패 조건 |
| [5. Field keyword](#5-field-keyword) | `constant`·`minimum`·`maximum`·`when`·`otherwise`·`required`·`constraints`의 평가 |
| [6. `commands[]`와 `flags[]`](#6-commands와-flags) | command 항목의 키, flag 선언, payload 정책, direction이 schema에 없는 이유 |
| [7. Durable format과 logical stream](#7-durable-format과-logical-stream) | `durableFormats[]`와 `relocationLogicalStreamFormat`의 header·checksum·fixture 선언 |
| [8. Semantic 선언](#8-semantic-선언) | `semanticContexts`, `semanticConstraints`, `relocationStateMachine`이 무엇을 고정하는가 |
| [9. Profile과 소유 조항](#9-profile과-소유-조항) | profile 15개가 어느 스펙 절의 규범을 기계용으로 고정한 것인지 |
| [10. 검증 요구](#10-검증-요구) | dialect가 바뀔 때 무엇으로 확인하는가 |

## 1. Dialect와 소비자

`schemaDialect`는 `zlink-service-wire-schema-v1`, `schemaVersion`은 `1`이다. 이 두 값은 이 장이
설명하는 읽기 규칙의 식별자다. 읽기 규칙이 바뀌면 dialect 값이 바뀌고, 값만 바뀌면(command
추가, bound 조정, field 추가) dialect는 그대로다.

schema를 읽는 소비자는 셋이다.

| 소비자 | 무엇을 읽는가 | 어디에 있는가 |
|---|---|---|
| Validator | schema 전체. 참조 무결성, 길이 용량, closed union, TLV 순서, durable checksum, semantic 선언의 literal 일치를 검사한다 | `validate-service-wire-schema.mjs` |
| Generator | `commands`·`flags`·일부 `types`(enum)·`bounds`·`frameworkMultipartV1Profile`·`terminal-failure-integrity`. 네 언어의 상수 표와 공통 decoder fixture를 낸다 | `generate-service-wire-assets.mjs`, `generate-service-wire-pilot-codecs.mjs` |
| Runtime codec | `types`·`commands`·`durableFormats`·`relocationLogicalStreamFormat`의 layout. 생성되었든 손으로 썼든 이 장의 규칙으로 bytes를 만들고 읽는다 | 각 언어 runtime |

Validator가 통과시킨 schema만 기준이다. 통과하지 못한 schema로 만든 codec·fixture는 계약이
아니다.

**문장형 문자열 값.** profile과 semantic 선언에는
`source-memory-payload-is-the-only-handoff-source-for-work-and-timers-accepted-before-capture`처럼
문장을 하이픈으로 이은 문자열 값이 있다. 이 값은 자유 산문이 아니다. Validator가 기대값과
byte 단위로 비교하는 **literal**이며, 그 literal이 뜻하는 규범은 [§9](#9-profile과-소유-조항)의
소유 조항이 산문으로 정한다. 따라서 이런 값을 바꾸는 일은 소유 조항을 바꾸는 일이고, 소유
조항을 바꾸는 일은 이 값을 바꾸는 일이다 — 한쪽만 바뀐 변경은 validator 또는 리뷰에서 거부된다.

## 2. 최상위 키

`schemaDialect`·`schemaVersion` 두 식별자([§1](#1-dialect와-소비자))를 뺀 26개 키는 역할이 네 가지다.

**Protocol 상수** — record 하나하나에 공통인 값.

| 키 | 선언 |
|---|---|
| `protocol` | `name`, `magic`(byte 배열), `wireMajor`, `byteOrder`, `headPrefixBytes`, `requiredCapability`. [06 §2](06-wire-protocol.ko.md#2-record-framing과-decode)의 head prefix가 이 값을 그대로 쓴다 |
| `bounds` | 이름 있는 양의 정수 한도의 목록. `{ "name", "value" }`. layout이 `$bound`로 가리킨다([§3](#3-모든-layout에-공통인-규칙)) |
| `flags` | head prefix flags byte의 bit 선언([§6](#6-commands와-flags)) |
| `reservedCommandRanges` | 할당하지 않는 command ID의 닫힌 구간 `{ "first", "last" }` 목록 |

**Layout 선언** — bytes의 배치.

| 키 | 선언 |
|---|---|
| `types` | 이름 있는 layout의 목록. 모든 layout은 `name`과 `kind`를 갖고, kind가 나머지 키를 정한다([§4](#4-typeskind-열-가지)) |
| `commands` | command 항목의 목록. `body`가 head prefix 뒤 bytes의 배치다([§6](#6-commands와-flags)) |
| `durableFormats` | 저장소에 남는 envelope 네 종의 header·body·checksum 선언([§7](#7-durable-format과-logical-stream)) |
| `relocationLogicalStreamFormat` | source에서 target으로 직접 흐르는 relocation logical stream의 선언([§7](#7-durable-format과-logical-stream)) |

**Semantic 선언** — bytes 밖의 규칙을 기계가 읽을 수 있게 고정한 것.

| 키 | 선언 |
|---|---|
| `semanticContexts` | layout 밖에서 오는 discriminator 값의 이름·형·출처([§8](#8-semantic-선언)) |
| `semanticConstraints` | lifecycle·fence·순서 규칙의 literal tuple 목록. kind마다 하나([§8](#8-semantic-선언)) |
| `relocationStateMachine` | relocation phase의 commit 순서·전이·command별 규칙([§8](#8-semantic-선언)) |

**Profile 선언** — 다른 스펙 절이 산문으로 소유한 규범을 기계용으로 고정한 것. 15개이며
[§9](#9-profile과-소유-조항)의 표가 소유 절을 연결한다.

## 3. 모든 layout에 공통인 규칙

**Byte order.** `protocol.byteOrder`와 각 `durableFormats[].byteOrder`가 허용하는 값은 `big-endian`
하나다. 다른 byte order를 선언할 자리는 없다. Wire의 byte 순서 규칙 자체는
[06 §2](06-wire-protocol.ko.md#2-record-framing과-decode)가 소유한다.

**정수 encoding.** `integer` kind의 `encoding`은 `u8`, `u16`, `u32`, `u64`, `i64` 다섯 가지이고 모두
고정폭이다. 가변 길이 정수는 이 dialect에 없다. 길이·개수·discriminator·field ID는 전부 이
다섯 가지 중 하나를 `$ref`로 가리키고, durable checksum은 `encoding: "u32-big-endian"`으로 같은
폭을 직접 적는다([§7.1](#71-durableformats)).

**`$ref`.** `types[].name`을 가리킨다. 선언되지 않은 이름을 가리키면 validator가 schema를 거부한다.
`$ref` 자리의 bytes는 그 이름의 layout과 같다 — 별도의 header나 길이 prefix가 추가되지
않는다(길이가 필요하면 가리킨 layout이 자기 kind로 선언한다).

**`$bound`.** `bounds[].name`을 가리킨다. `maximumBytes`, `maximumItems`, `maximumEncodedBytes`,
`maximum` 자리에 숫자 대신 온다. 선언된 bound는 적어도 한 곳에서 참조되어야 하고, 참조되지 않는
bound는 validator가 거부한다. 같은 한도를 두 곳에서 다른 숫자로 쓰지 않기 위한 장치다.

**크기 상한.** `maximumEncodedBytes`는 layout 전체의 encoded 상한이며, validator는 kind별 구성
요소의 상한 합이 이 값을 넘지 않는지 계산한다. `runtimeMaximumBytes`·`runtimeMaximumEncodedBytes`는
topology별로 협상된 상한(`$negotiatedBound`)을 선언하는 자리이고, 협상값은 항상
`absoluteMaximum`(`$bound`) 이하다. 상한을 어떤 순서로 확인하고 넘으면 어떻게 거부하는지는
[06 §2](06-wire-protocol.ko.md#2-record-framing과-decode)가 소유한다.

**실패의 종류.** 이 장이 "실패"라고 적은 조건은 모두 [06 §2](06-wire-protocol.ko.md#2-record-framing과-decode)의
protocol error다. Encoder는 같은 선언을 어기는 입력을 bytes로 만들지 않는다.

**Encoder와 decoder는 같은 선언을 읽는다.** 어떤 keyword도 "encoder만" 또는 "decoder만"을 위한
것이 아니다. Encoder는 선언대로 bytes를 만들고, decoder는 선언과 다른 bytes를 거부한다. 이 장이
keyword마다 두 쪽을 따로 적는 이유는 규칙이 다르기 때문이 아니라 위반이 드러나는 자리가 다르기
때문이다.

## 4. `types[].kind` 열 가지

각 kind의 선언 키와 byte 배치다. "실패"는 [§3](#3-모든-layout에-공통인-규칙)의 protocol error다.

### 4.1 `integer`

| 키 | 뜻 |
|---|---|
| `encoding` | `u8`·`u16`·`u32`·`u64`·`i64` 중 하나 |
| `minimum`, `maximum` | 값의 범위. 숫자 또는 `$bound`. encoding의 표현 범위보다 좁을 때만 적는다 |

배치는 encoding 폭의 big-endian 정수 하나다. 실패: bytes 부족, 범위 밖의 값.

### 4.2 `enum`

| 키 | 뜻 |
|---|---|
| `encoding` | 정수 encoding |
| `values` | `{ "name", "value" }` 목록. name과 value 모두 중복이 없다 |

배치는 encoding 폭의 정수 하나이고 `values`에 있는 value만 유효하다. 실패: bytes 부족, 선언되지
않은 value. 이름은 wire에 나가지 않는다 — 생성된 상수 표와 fixture가 이름을 쓴다.

### 4.3 `length-prefixed-bytes`

| 키 | 뜻 |
|---|---|
| `lengthType` | 길이 prefix의 정수 layout(`$ref`) |
| `minimumBytes`, `maximumBytes` | 길이의 범위. `maximumBytes`는 대개 `$bound` |
| `runtimeMaximumBytes` | topology별 협상 상한([§3](#3-모든-layout에-공통인-규칙)) |
| `zeroLengthMeaning` | `absent`. 길이 0이 "값 없음"을 뜻하는 optional layout에만 적는다 |

배치는 길이 prefix 뒤 정확히 그 길이의 raw bytes다. 실패: prefix 부족, 길이가 범위 밖, bytes
부족. `zeroLengthMeaning`이 없으면 길이 0은 빈 값이지 없는 값이 아니다.

### 4.4 `length-prefixed-text`

`length-prefixed-bytes`의 키에 다음을 더한다.

| 키 | 뜻 |
|---|---|
| `encoding` | `utf-8` |
| `nul` | `forbidden`. NUL byte를 담지 않는다 |

배치는 같다. 실패에 잘못된 UTF-8 sequence와 NUL byte가 더해진다. 길이는 code point 수가 아니라
byte 수다.

### 4.5 `struct`

| 키 | 뜻 |
|---|---|
| `fields` | field 목록. 선언 순서가 byte 순서다([§5](#5-field-keyword)) |
| `constraints` | 여러 field에 걸친 제약. `not-both-zero`(`fields`), `field-less-than-or-equal`(같은 struct의 두 field) |
| `maximumEncodedBytes` | encoded 상한 |
| `trailingBytes` | `forbidden`. 길이가 밖에서 정해지는 struct에서 남는 byte를 거부한다 |
| `presence`, `scope`, `storage`, `metadataMeaning`, `queueMeaning` | 이 layout이 어디에 나타나고 무엇을 뜻하는지 적은 literal. 바이트 배치에 영향이 없고 [§8](#8-semantic-선언)·[§9](#9-profile과-소유-조항)의 규칙이 참조한다 |

배치는 `fields`를 선언 순서로 이어 붙인 것이다. 구분자·padding·정렬은 없다. `when`이 거짓인
field는 bytes에 존재하지 않는다([§5](#5-field-keyword)). 실패: 어느 field의 실패, constraint 위반,
`trailingBytes: forbidden`인데 byte가 남음.

### 4.6 `vector`

| 키 | 뜻 |
|---|---|
| `countType` | 개수 prefix의 정수 layout |
| `maximumItems` | 개수 상한. 숫자 또는 `$bound` |
| `item` | item layout(`$ref`) |
| `constraints` | `sorted`, `unique`. `sorted`는 `comparison`을 반드시 갖는다 — `canonical-authority-key-bytes`, `utf-8-bytes`, `wire-value`, `wire-value-then-utf-8-bytes`, `unsigned-wire-value`. `unique`는 비교 대상을 `field`(하나) 또는 `fields`(여럿)로 지정하고 `comparison`은 선택이다 |
| `participantIdDerivation` | item의 participant 식별자를 어떻게 얻는지 적은 literal. 배치에 영향이 없다 |

배치는 개수 prefix 뒤 item이 개수만큼 반복된다. `sorted`는 선언한 comparison으로 오름차순임을,
`unique`는 같은 값이 둘 없음을 요구한다. 이 두 제약은 canonical encoding을 위한 것이므로
decoder는 정렬되지 않았거나 중복인 vector를 **고쳐 읽지 않고 거부한다.** 실패: 개수 부족·상한
초과, item 실패, 정렬·중복 위반.

### 4.7 `versioned-vector`

| 키 | 뜻 |
|---|---|
| `layout` | 정확히 세 항목 — `constant`를 가진 version field, `counts`로 반복 field를 가리키는 count field, `kind: "repeat"`·`countFrom`·`item`을 가진 반복 field |
| `constraints`, `maximumEncodedBytes`, `trailingBytes` | `vector`·`struct`와 같다 |

배치는 version, count, item 반복 순서다. 실패: version 불일치, `vector`의 실패, 남는 byte.
`metadata-frame`처럼 자기 frame을 통째로 차지하는 layout이 이 kind를 쓴다.

### 4.8 `versioned-length-delimited`

| 키 | 뜻 |
|---|---|
| `version` | `constant`를 가진 정수 field |
| `length` | 정수 layout과 `covers: "body"`. 길이가 덮는 범위는 body 전체다 |
| `body` | field 목록. `struct`의 `fields`와 같다 |
| `maximumEncodedBytes`, `runtimeMaximumEncodedBytes`, `trailingBytes` | 위와 같다 |
| `constraints` | terminal envelope의 모양 제약 — `terminal-success-shape`, `terminal-failure-shape`, `existing-has-no-application-payload` |
| `correlationFields`, `description` | 어느 field가 상관 식별자인지 적은 literal과 설명. 배치에 영향이 없다 |

배치는 version, length, body 순서다. Decoder는 length를 읽고 상한을 확인한 뒤 정확히 그 길이만큼
body를 읽는다. body가 length보다 짧거나 길면 실패다. 이 kind는 뒤에 오는 bytes와의 경계가 length
field로 정해지므로 typed payload envelope처럼 다른 frame에 실리는 layout이 쓴다.

### 4.9 `conditional-union`

| 키 | 뜻 |
|---|---|
| `discriminators` | case를 고르는 값의 목록. 각 항목의 `source`가 출처를 정한다 — 문자열 `"wire"`(이 layout 바로 앞에서 읽는 값, 같은 항목의 `$ref`가 layout), 객체 `{ "enclosingField": "<field>" }`(이 union을 담은 struct에서 이미 읽은 field), 객체 `{ "context": "<name>" }`(`semanticContexts`의 이름) |
| `bodyLengthType`, `bodyLengthCovers` | 선택한 case 앞에 오는 길이 prefix와 그 범위(`selected-case`). 없는 union은 길이 prefix 없이 case가 바로 온다 |
| `cases` | `{ "when": { discriminator: value, … }, "fields": [...] }` 목록. 모든 discriminator를 정확히 한 번씩 배정한다 |
| `otherwise` | 어느 case에도 맞지 않을 때 — `protocol-error`(거부) 또는 명시적 field 목록 `{ "fields": [...] }`(현재 schema는 빈 목록만 쓴다) |
| `trailingBytes` | `forbidden` |
| `presence`, `release` | 나타나는 조건과 해제 조건을 적은 literal. 배치에 영향이 없다 |

배치는 `source: wire`인 discriminator, (선언했으면) body length, 선택한 case의 fields 순서다.
`enclosingField`·`context` discriminator는 bytes를 차지하지 않는다. 실패: discriminator 실패,
`otherwise: protocol-error`인데 맞는 case 없음, length가 case와 불일치, 남는 byte.

### 4.10 `tlv32`

| 키 | 뜻 |
|---|---|
| `totalLengthType` | 전체 길이 prefix의 정수 layout |
| `fieldIdType`, `fieldLengthType` | 각 field의 ID와 길이 prefix의 정수 layout |
| `fields` | `{ "id", "name", "$ref", "required", … }` 목록 |
| `encodingOrder` | `ascending-field-id`. field는 ID 오름차순으로만 나온다 |
| `duplicateField` | `protocol-error`. 같은 ID가 둘이면 거부한다 |
| `unknownField` | `skip-after-length-validation`. 모르는 ID는 길이를 확인하고 건너뛴다 |
| `maximumEncodedBytes`, `trailingBytes` | 위와 같다 |

배치는 total length 뒤 (field ID, field length, field value)의 반복이다. 이 kind만 field를
생략·추가할 수 있으므로 확장 가능한 descriptor에 쓴다. 실패: total length 밖으로 나가는 field,
순서 위반, 중복 ID, `required: true`인 field 부재, 알려진 field의 value 실패. 모르는 field는
실패가 아니다 — 그 field의 length만큼 건너뛴다.

## 5. Field keyword

`struct.fields`, `versioned-length-delimited.body`, `conditional-union.cases[].fields`,
`commands[].body`의 field 항목이 쓰는 keyword다.

| keyword | 뜻 | Encoder | Decoder |
|---|---|---|---|
| `name` | field 이름. 같은 field 목록 안에서 유일하다 | — | — |
| `$ref` | layout([§3](#3-모든-layout에-공통인-규칙)) | 그 layout으로 쓴다 | 그 layout으로 읽는다 |
| `constant` | 이 field가 가져야 하는 값(version, magic 등) | 그 값을 쓴다 | 다른 값이면 실패 |
| `minimum`, `maximum` | `$ref`가 정수일 때 그 field에만 적용되는 더 좁은 범위 | 범위 밖 값을 거부한다 | 범위 밖 값이면 실패 |
| `when` | 이 field가 존재하는 조건. `fieldPresent: "<earlier field>"`, `fieldEquals: { "name": "<earlier field>", "value": … }`, 또는 head prefix의 flag를 보는 `allFlagsSet: [...]`·`anyFlagsSet: [...]`. 참조 대상은 **앞에 선언된 field와 flag**뿐이다 | 조건이 거짓이면 field를 쓰지 않는다 | 조건이 거짓이면 읽지 않는다. 조건은 이미 읽은 값으로만 판정된다 |
| `otherwise` | `when`이 거짓일 때 — `forbidden` | 조건이 거짓인데 값이 주어지면 거부한다 | (조건이 거짓이면 bytes를 소비하지 않으므로 위반은 뒤 field의 실패로 드러난다) |
| `required` | `tlv32`에서만. `true`면 반드시 있어야 한다 | 없는 required field를 만들지 않는다 | 없으면 실패 |
| `id` | `tlv32`에서만. field ID | — | — |
| `constraints` | field 하나에 대한 제약. 현재 `contains-protocol-required-capability`(text vector가 `protocol.requiredCapability`를 담아야 한다) | 위반 입력을 거부한다 | 위반이면 실패 |
| `description` | 값의 뜻을 적은 설명 문자열. 배치와 검증에 영향이 없다 | — | — |

`when`이 앞 field와 flag만 참조하는 규칙은 dialect의 성질이다 — 한 번의 순방향 decode로 모든
조건이 판정되고, 두 번째 pass나 backtracking이 필요하지 않다.

## 6. `commands[]`와 `flags[]`

### 6.1 Command 항목

| 키 | 뜻 |
|---|---|
| `id` | 0이 아닌 u8 값. 유일하고 `reservedCommandRanges` 밖이다 |
| `name` | 유일한 이름. 생성된 상수 표가 쓴다 |
| `domain` | `application` 또는 `infrastructure`. 두 값의 dispatch 차이는 [06 §3](06-wire-protocol.ko.md#3-command-space)이 소유한다 |
| `allowedFlags`, `requiredFlags` | 이 command가 세울 수 있는 flag와 반드시 세워야 하는 flag. `requiredFlags ⊆ allowedFlags` |
| `flagConstraints` | flag 사이의 관계 — `all-or-none`(나열한 flag는 전부 세우거나 전부 내린다), `implies` |
| `body` | head prefix 바로 뒤 bytes의 field 목록([§5](#5-field-keyword)) |
| `payload` | `forbidden`·`optional`·`required`. typed payload envelope frame이 오는지 |
| `payloadType` | `payload`가 `forbidden`이 아닐 때 그 frame의 layout(`$ref`) |
| `semanticConstraints` | 이 command에 걸린 semantic 규칙의 이름 목록([§8](#8-semantic-선언)) |

**Direction은 schema에 없다.** 어느 쪽이 어느 command를 보낼 수 있는지, 어느 topology에서
허용되는지는 runtime 규칙이며 [06 §3](06-wire-protocol.ko.md#3-command-space)·[§4](06-wire-protocol.ko.md#4-admission과-connection-fence)와
`relocationStateMachine.commandRules`가 소유한다. Decoder가 bytes를 읽는 일과, 읽은 command를 이
연결에서 받아도 되는지 판정하는 일은 다른 계층이다.

**Reply 관계도 항목에 없다.** 어느 request에 어느 reply가 대응하는지는 body의 correlation
field([§4.8](#48-versioned-length-delimited)의 `correlationFields`)와 `relocationStateMachine`이
선언한다. `replyTo` 같은 일반 키는 없다.

### 6.2 Flag 선언

| 키 | 뜻 |
|---|---|
| `name` | flag 이름 |
| `bit` | flags byte 안의 bit. 2의 거듭제곱이고 유일하다 |
| `frame` | 이 flag가 세워지면 따라오는 frame의 layout(`$ref`) |
| `whenSet`, `whenClear` | `frame-required`·`frame-forbidden`. 세워지면 그 frame이 반드시 있고, 내려가면 없어야 한다 |
| `controls` | frame이 아니라 command body의 어느 부분(예: tail·extension)을 켜는 flag임을 적은 literal. body field의 `when`이 이 flag를 조건으로 삼는다 |

선언되지 않은 bit, `allowedFlags` 밖의 bit, `requiredFlags`의 부재, `flagConstraints` 위반,
`whenSet`·`whenClear`와 다른 frame 개수는 모두 [§3](#3-모든-layout에-공통인-규칙)의 실패다. flag가
record의 frame 수를 정하는 방식은 [06 §2](06-wire-protocol.ko.md#2-record-framing과-decode)가 소유한다.

## 7. Durable format과 logical stream

### 7.1 `durableFormats[]`

저장소에 남아 프로세스 수명을 넘는 envelope다. 항목마다 다음을 선언한다.

| 키 | 뜻 |
|---|---|
| `name` | format 이름 |
| `magic`, `formatVersion` | envelope 앞의 magic bytes와 버전 |
| `flags`, `flagsType` | flags 값과 그 정수 layout |
| `byteOrder` | big-endian |
| `bodyLengthType`, `body`, `maximumEncodedBytes` | body 길이 prefix, body layout(`$ref`), 상한 |
| `checksum` | `algorithm`(`crc32c-castagnoli`), `encoding`, `coverage`(예: `magic-through-body`), `position`(`trailing`) |
| `goldenFixture` | 이 format의 정상·거부 bytes를 고정한 fixture 경로 |
| `providerInterpretation` | `opaque-bytes`. 저장소 provider는 이 bytes를 해석하지 않는다 |

배치는 magic, formatVersion, flags, body length, body, checksum 순서이고 checksum은 `coverage`가
정한 범위를 덮는다. Decoder는 magic·version·length·checksum을 **모두** 확인한 뒤 body를 읽는다.
어느 하나의 불일치도 실패이며, 부분 복원은 없다. 각 format이 저장소 안에서 갖는 의미(언제 쓰고
언제 지우는가)는 [06 §7](06-wire-protocol.ko.md#7-durable-authority와-explicit-creation)·[§9](06-wire-protocol.ko.md#9-maintenance-capture와-relocation-envelope)가
소유한다.

### 7.2 `relocationLogicalStreamFormat`

Source memory에서 target으로 직접 전달되는 relocation payload의 선언이다. `body`(`$ref`)가 logical
stream 전체의 layout이고, `generatedObjectTree`는 그 안에서 runtime이 따로 다루는 부분
tree(application state, saved work, timer)를 `$ref`로 가리킨다. `maximumBytes`는 logical 상한,
`chunkSplit`·`replay`는 stream을 chunk로 나누어 보내고 다시 이을 때의 규칙 literal이며,
`goldenFixture`가 bytes를 고정한다. Chunk command와 checksum 규약은
[06 §9](06-wire-protocol.ko.md#9-maintenance-capture와-relocation-envelope)가 소유한다.

## 8. Semantic 선언

### 8.1 `semanticContexts`

Layout 밖에서 오는 값의 선언이다. 항목마다 `name`, 값의 형(`valueType`의 `$ref` 또는
`valueKind: boolean`), `source`(그 값을 어디서 얻는지 적은 literal)를 갖는다.
`conditional-union`의 `source: context` discriminator가 이 이름을 참조한다. 즉 bytes에는 없지만
decoder가 이미 알고 있는 값(예: 원래 operation의 종류)으로 case를 고르는 장치다. Decoder는 context
값을 **bytes를 읽기 전에** 갖고 있어야 하며, 없으면 그 union을 읽을 수 없다.

### 8.2 `semanticConstraints`

kind마다 하나인 object 목록이다. `implies`는 `if`/`then`에 `contextEquals`를 두는 일반 관계이고,
나머지 kind는 각각 하나의 규칙 묶음(authority 상태 조합, saved work의 소유, relay의 terminal 소유,
admission fence 등)을 field 이름과 literal 값의 tuple로 고정한다.

Validator는 각 kind의 object를 자기 코드의 기대값과 **literal로 비교**한다. 따라서 이 목록은
문서용이 아니라 build-time에 검사되는 데이터다 — 규칙이 바뀌면 validator의 기대값과 schema를
함께 바꿔야 하고, 한쪽만 바뀐 schema는 통과하지 못한다. 반면 이 tuple을 runtime decoder의 검사로
내리는 것은 `terminal-failure-integrity`(terminal result·failure code·payload의 허용 조합) 하나다.
나머지 kind의 규범은 [§9](#9-profile과-소유-조항)의 소유 절이 정하고 runtime이 그 절에 따라
구현하며, schema는 그 규범을 기계가 대조할 수 있는 형태로 보존한다.

### 8.3 `relocationStateMachine`

`phaseType`(phase enum의 `$ref`), `authorityCommitOrder`(phase마다 그 앞에 끝나야 하는 일과 relocation
record의 존재 여부), `transitions`(허용 전이), `commandRules`(command마다 보낼 수 있는 phase와
역할)를 선언한다. 전이의 의미는 [06 §10](06-wire-protocol.ko.md#10-relocation-actor-membership과-ready)과
[Location runtime](../05-location-relocation/01-location-runtime.ko.md)이 소유하고, 이 선언은 그
규칙을 validator가 command 선언과 대조할 수 있게 고정한 것이다.

## 9. Profile과 소유 조항

Profile 키는 다른 절이 산문으로 소유한 규범을 기계용 literal로 고정한다. 값의 뜻을 알려면 소유
절을 읽고, 소유 절을 바꾸면 profile을 함께 바꾼다([§1](#1-dialect와-소비자)).

| Profile 키 | 소유 조항 |
|---|---|
| `frameworkJsonV1Profile` | [Message model §5](../00-foundation/05-message-model.ko.md#5-framework-json-v1-typed-payload-profile) |
| `frameworkMultipartV1Profile` | [06 §2 "Framework multipart application profile"](06-wire-protocol.ko.md#2-record-framing과-decode) |
| `authorityKeyFormat` | [06 §1 "Location Store authority key 형식"](06-wire-protocol.ko.md#1-schema와-생성-경계) |
| `authorityStoreAccessProfile` | [Location runtime §6.1](../05-location-relocation/01-location-runtime.ko.md#61-read와-cas) |
| `descriptorEnumerationProfile` | [Location runtime §4](../05-location-relocation/01-location-runtime.ko.md#4-실행-중인-node와-제공-기능을-찾는다) · [§6.2](../05-location-relocation/01-location-runtime.ko.md#62-여러-페이지를-같은-시점의-목록으로-읽는다) |
| `authorityStoreGenerationProfile` | [Location runtime §3.1~§3.3](../05-location-relocation/01-location-runtime.ko.md#3-같은-id의-재생성과-owner-변경을-구분하는-값) |
| `authorityStoreDurabilityProfile` | [Location runtime §3.3](../05-location-relocation/01-location-runtime.ko.md#33-현재-위치-record에-저장하는-값) · [§11](../05-location-relocation/01-location-runtime.ko.md#11-host가-종료될-때-store-record를-정리한다) |
| `ownerLeaseAuthorityProfile` | [Location runtime §4.1](../05-location-relocation/01-location-runtime.ko.md#41-대상-descriptor의-owner-lease-검증) · [§5](../05-location-relocation/01-location-runtime.ko.md#5-store-연결이-끊기면-이전-owner의-새-작업을-막는다) |
| `relocationRetentionPolicy` | [Redis Relocation Store §4](../05-location-relocation/03-relocation-store-redis.ko.md#4-operation별-결과) · [§5](../05-location-relocation/03-relocation-store-redis.ko.md#5-취소-오류와-결과-재구성) · [§6](../05-location-relocation/03-relocation-store-redis.ko.md#6-payload-게시와-정리) |
| `relocationStorageProfile` | [Redis Relocation Store §2](../05-location-relocation/03-relocation-store-redis.ko.md#2-공개-spi와-책임-경계)~[§6](../05-location-relocation/03-relocation-store-redis.ko.md#6-payload-게시와-정리) · [06 §9](06-wire-protocol.ko.md#9-maintenance-capture와-relocation-envelope) |
| `relocationTransferChecksumProfile` | [06 §9 "CRC-32C 규약과 capability"](06-wire-protocol.ko.md#9-maintenance-capture와-relocation-envelope) |
| `maintenanceAdmissionProfile` | [Host relocation flow §4](../05-location-relocation/05-host-relocation-flow.ko.md#4-target을-선택하기-전에-확인하는-조건) · [§5](../05-location-relocation/05-host-relocation-flow.ko.md#5-mode에-맞는-target을-선택한다) |
| `terminationResultProfile` | [06 §11](06-wire-protocol.ko.md#11-request-terminal-identity) |
| `livenessProfile` | [06 §5](06-wire-protocol.ko.md#5-service-liveness) · [Transport liveness §3](05-transport-liveness.ko.md#3-routemesh와-clientserver) |
| `fanoutLivenessProfile` | [06 §5](06-wire-protocol.ko.md#5-service-liveness) · [Transport liveness §4](05-transport-liveness.ko.md#4-classic-fanout) |

`durableFormats`·`relocationLogicalStreamFormat`·`relocationStateMachine`은 profile이 아니라 layout·
semantic 선언이며 소유 절은 [§7](#7-durable-format과-logical-stream)·[§8](#8-semantic-선언)에 적었다.

## 10. 검증 요구

Dialect 변경(새 최상위 키, 새 `kind`, 새 keyword, keyword 평가 규칙의 변경)은 다음을 모두 갖춘다.

- Validator가 그 키·kind·keyword를 해석하고, `--self-test`가 그 규칙을 어기는 mutation을 실제로
  거부한다([06 §1 "Validator"](06-wire-protocol.ko.md#1-schema와-생성-경계)).
- 이 장의 해당 절이 같은 변경에서 갱신된다. schema에 있는데 이 장에 없는 키·kind·keyword는
  이 장의 결함이다.
- 새 layout kind는 golden fixture로 정상 bytes와 거부 bytes를 함께 고정한다([06 §12](06-wire-protocol.ko.md#12-검증-요구)).

값 변경(command·field·bound·enum 추가)은 dialect 변경이 아니며 [06 §12](06-wire-protocol.ko.md#12-검증-요구)의
요구만 따른다.

---

[Channel·Transport 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. Service wire protocol](06-wire-protocol.ko.md)
