# Node framework-json-v1 encode 구조 판정

**현재의 JSON.stringify + replacer를 유지한다.** 직접 순회하는 single writer를 scratch에서 비교했으나, 실제 UTF-8 소비까지 포함한 할당 비용이 줄지 않았다. CPU 반복값도 변동이 커 개선을 확정할 수 없다. 의미 판정 위치는 아래 집계에서 **35 → 34**로 줄일 수 있지만 비용 조건을 충족하지 못했다. 따라서 production 구현·WebGL 생성물 변경·commit을 하지 않았다.

## 범위와 조건

- Primary: `/root/worktree/zlink-v21-s033/n4w`, branch `perf/4354-node-json-writer`, HEAD `f723033a5a524af5dcfa4c04fc42677596c5f429`. 시작·종료 모두 clean이다. [검증 기록](raw/validation-summary.json).
- Scratch: `/root/worktree/zlink-v21-s033/n4w-scratch`. `ae8a5178b5` archive에 이 branch의 Node packages를 복사하고 자체 dependency 설치·공식 `npm run build`를 실행했다. 비교 대상 TS 원문 일치, compiled artifact SHA와 source보다 늦은 build 시간을 확인했다. [무결성](raw/baseline-source-integrity.json), [build](raw/build-baseline.log), [호출 비교](call-shape.md).
- Node **22.23.3**. 모든 profile process와 하위 thread는 CPU **6-7,14-15**를 상속했다. L4는 start·mid-active의 모든 thread에서 affinity와 SCHED_OTHER를 확인했다. `measure.lock`, 다른 worktree/output, GitHub, push, PR, internal-docs.sh, sub-agent, 다른 AI 도구를 사용하지 않았다.
- L4는 process마다 3초 warmup + 4초 active. 별도 discarded warm run을 보존했다. Micro는 process마다 0.7초 warmup + 1.8초 active. 모든 L4 process pair lifetime은 **8.56–9.21초**, micro lifetime은 최대 **3.32초**로 20초 이내다. [전체 lifetime·종료 코드](raw/validation-summary.json).
- CPU profile와 Inspector heap sampling(16 KiB, minor/major GC로 회수한 객체 포함)을 함께 사용했다. 이 결과는 **비용의 형태 비교**이며 throughput·binding ratio 판정이 아니다. Heap sampling은 V8 heap의 근삿값이며 external byte 저장소 전체를 포함하지 않는다. [수집기](tools/allocation-preload.cjs).
- 최초 HTTP client materialize는 local package 누락으로 실패했다. 이 scratch에서 bootstrap compiler와 registry binding으로 package를 만들고 설치·build를 완료했다. 실패 원본을 포함해 raw 파일을 삭제하지 않았다. [최초 실패](raw/materialize-http.log), [설치](raw/npm-ci.log).

## 현재 encode 판정과 규칙 수

아래 `S`는 `framework/languages/node/packages/stream-wire/src/internal/framework-json-v1.ts`, `A`는 `framework/languages/node/packages/framework/src/runtime/messaging/framework-json-v1.ts`다. [기준 원문과 line](raw/baseline-source.txt).

현재 default는 `S:37`의 JSON.stringify가 각 property를 Get하고 toJSON을 실행한 뒤, `S:19-24`의 replacer가 holder의 같은 property를 **다시 Get**하여 정책을 적용한다. 따라서 정책의 `source`와 JSON.stringify가 받은 `item`은 다를 수 있다. Custom replacer는 `S:52-55`에서 toJSON 결과를 callback에 넘기고, callback이 반환한 값 하나를 source/item 양쪽으로 사용한다.

규칙 수는 입력 허용·거부, wire 표현, 관찰 가능한 접근 순서를 정하는 **판정 위치**를 센다. 아래 표가 집계 범위다. 매개변수가 있는 escape mapping은 한 소유 mapping으로 세며, boxed 종류별 판정은 네 위치, boolean 판정은 두 위치다. Native object/array의 cycle 판정은 실제로 두 알고리즘에 있으므로 두 위치로 센다. Hook 내부의 임의 사용자 코드, GC, 엔진 최적화 분기는 세지 않는다. 서로 다른 engine 내부 branch 총수를 비교한 수치가 아니다.

Native 위치는 현재 `S:37,52`가 위임하는 동작이다. 해당 동작은 [ECMAScript JSON 직렬화 절차](https://tc39.es/ecma262/multipage/structured-data.html#sec-serializejsonproperty)와 대조하고 [원문](raw/ecma-stringify.txt)을 보존했다. 실제 허용·거부와 출력은 이 Node runtime에서 직접 대조했다.

| ID | 판정 | 현재 위치 / 결과 | 현재 | writer 위치 / 수 |
|---|---|---|---:|---|
| 01 | 원래 root가 undefined/function/symbol인가 | `S:32-33`: callback 전에 TypeError | 1 | `writer.cjs:45` / 1 |
| 02 | custom replacer가 주어졌는가 | `S:35-38,24,52-55`: default는 source 재조회, custom은 callback 반환값에 정책 | 1 | `:53-54` / 1 |
| 03 | schema가 있는가 | `S:39-40`: 완성된 JSON을 parse한 뒤 기존 validator | 1 | `:92` / 1 |
| 04 | source가 non-finite primitive number인가 | `S:63-65`: NaN/±Infinity TypeError | 1 | `:33` / 1 |
| 05 | source가 primitive bigint인가 | `S:66`: 10진 문자열 표현 선택 | 1 | `:34-36` / 1 |
| 06 | bigint가 signed 최소~unsigned 최대 범위인가 | `S:5-6,67-70`: −2^63..2^64−1 허용, 밖은 RangeError | 1 | `:35-36` / 1 |
| 07 | source가 이 realm의 Uint8Array인가 | `S:72-74` → `A:7-10`: Buffer 포함, 정확한 byte range를 native padded Base64로 변환 | 1 | `:38,29-30` / 1 |
| 08 | source가 이 realm의 Date인가 | `S:75-77`: TypeError. Native toJSON은 그 전에 실행될 수 있음 | 1 | `:39` / 1 |
| 09 | source가 callable toJSON을 가진 object인가 | `S:78-84`: TypeError. Uint8Array는 앞 규칙에서 이미 반환 | 1 | `:40` / 1 |
| 10 | 첫 Get 값에 toJSON 조회를 해야 하는가 | Native SerializeJSONProperty, [원문](raw/ecma-stringify.txt):161: object/function/bigint | 1 | `:49-50` / 1 |
| 11 | 조회한 toJSON이 callable인가 | 같은 원문 `:162`: key를 인자로 먼저 호출, hook 예외 전파 | 1 | `:51` / 1 |
| 12 | boxed Number/String/Boolean/BigInt 중 무엇인가 | Native 내부 slot 판정, 같은 원문 `:171-177` | 4 | Node `util.types`, `:56-59` / 4 |
| 13 | raw JSON 객체인가 | Native 같은 원문 `:167-169`: Node 22 JSON.rawJSON 원문 삽입 | 1 | `:68` / 1 |
| 14 | null인가 | Native 같은 원문 `:180`: `null` | 1 | `:61` / 1 |
| 15 | boolean literal 선택 | Native 같은 원문 `:181-182`: true/false 두 판정 | 2 | boolean 분류 + 값 선택 `:63` / 2 |
| 16 | string인가 | Native 같은 원문 `:183`: 공통 quote로 값과 key 인코딩 | 1 | `:64` / 1 |
| 17 | number인가 | Native 같은 원문 `:184`: JSON number 변환 선택 | 1 | `:65` / 1 |
| 18 | 직렬화 단계의 number가 finite인가 | Native 같은 원문 `:184-185`: finite는 native 10진 표기, 나머지는 null | 1 | `:65` / 1 |
| 19 | 정책 뒤에도 bigint가 남았는가 | Native 같은 원문 `:187`: TypeError; boxed bigint도 여기에 해당 | 1 | `:66` / 1 |
| 20 | non-callable object를 직렬화할 것인가 | Native 같은 원문 `:188-192`: object 순회 | 1 | `:67` / 1 |
| 21 | JSON 표현이 없는 종류인가 | Native 같은 원문 `:194`: undefined 반환. undefined/function/symbol의 일반 동작 | 1 | `:88` / 1 |
| 22 | array인가, object인가 | Native 같은 원문 `:188-192` | 1 | `:72` / 1 |
| 23 | 현재 ancestor에 같은 객체가 있는가 | Native SerializeJSONObject `:335`, SerializeJSONArray `:374`: TypeError. 공유 subtree는 허용 | 2 | 두 container 공통 `:69` / **1** |
| 24 | object의 어떤 property를 열거하는가 | Native 같은 원문 `:341`: own enumerable string keys만. symbol/non-enumerable 제외 | 1 | Object.keys `:79` / 1 |
| 25 | object property 결과가 undefined인가 | Native 같은 원문 `:345`: 해당 property 생략 | 1 | `:81` / 1 |
| 26 | array의 길이와 읽을 범위 | Native 같은 원문 `:379-381`: length를 한 번 snapshot/ToLength, 0..length−1의 Get | 1 | `:73-75` / 1 |
| 27 | array item 결과가 undefined인가 | Native 같은 원문 `:382-385`: null 삽입. hole도 Get하므로 inherited index가 있으면 사용 | 1 | `:75` / 1 |
| 28 | quote/역슬래시/짧은 control escape mapping | Native QuoteJSONString, 같은 원문 `:196-315` | 1 | 공통 quote `:7-27` / 1 |
| 29 | 나머지 control/lone surrogate와 정상 unicode 처리 | 같은 QuoteJSONString: control/lone surrogate는 소문자 `\uXXXX`, 정상 pair·비ASCII·U+2028/2029는 유지 | 1 | `:20-24` / 1 |
| 30 | property key 순서 | Native enumeration: 일반 객체는 array-index 우선 오름차순, 나머지 생성 순서. Proxy는 ownKeys가 제공하는 유효 순서 | 1 | 같은 enumeration을 쓰는 Object.keys `:79` / 1 |
| **합계** | | | **35** | **34** |

Writer 경로의 line은 [단일 후보 원문](tools/writer.cjs) 기준이다. 이 수치에는 custom replacer를 처리하는 02를 이미 포함했다. 별도 serializer나 fallback 수를 0으로 숨겨 합산하지 않았다.

### 종류별 실제 의미

| 입력 / 관찰 지점 | 현재 동작과 후보 확인 |
|---|---|
| undefined/function/symbol | 원래 root는 거부. 일반 object property는 생략, array item은 null. 단 nested function의 callable toJSON이 scalar를 반환하면 그 scalar는 허용된다. |
| Map/Set | 별도 거부 규칙이 없다. own enumerable property를 직렬화한다. 일반 Map/Set은 `{}`. |
| Uint8Array 이외 typed array | 별도 거부 규칙이 없다. Int16Array는 numeric-key object, BigInt64Array item은 10진 문자열이다. Float64Array의 NaN item은 공용 primitive finite 규칙에서 거부한다. 다른 realm의 Uint8Array는 instanceof에 해당하지 않아 일반 numeric-key object다. |
| boxed primitive | boxed NaN/Infinity는 native unboxing 뒤 null. boxed bigint는 native TypeError. boxed Symbol은 보통 `{}`. Number/String 변환 override도 보존해야 한다. |
| getter | default는 native Get와 policy Get 두 번. 첫 값과 두 번째 값이 다르면 첫 item의 출력 또는 두 번째 source의 bytes/bigint/거부 정책이 적용된다. 한 번만 읽는 writer는 동치가 아니다. |
| proxy | native Get, toJSON 조회, instanceof의 prototype 검사, ownKeys, descriptor 열거 순서를 관찰할 수 있다. proxy를 탐지하여 별도 경로로 보내지 않았다. |
| cycle / sparse array / key mutation | cycle은 거부하고 같은 subtree의 재사용은 허용한다. array hole은 null 또는 inherited value다. object key 목록은 snapshot이며 getter가 나중 key를 삭제·추가할 때 native와 같은 Get 순서를 사용한다. |
| custom replacer | callback의 this는 실제 holder, root key는 빈 문자열이다. callback은 native toJSON 결과를 받는다. identity replacer에 Buffer를 주면 `{type:"Buffer",data:[...]}`이고, Date는 ISO string이 된다. callback이 Buffer를 반환하면 Base64 정책을 적용한다. 원래 root 거부는 callback으로 구제할 수 없으며, 허용 root를 callback이 undefined로 바꾸면 현재와 같이 결과가 undefined일 수 있다. |

위 결과의 실제 문자열·오류는 [관찰 기록](raw/observations.json), 동작 비교는 [corpus](raw/corpus.json), [test source](tools/corpus.cjs)에 있다. 완성된 JSON에 적용하는 schema validator와 decoder는 바꾸지 않았다.

## ONE writer 설계와 byte hook 제약

후보는 `property(holder,key)` 하나로 첫 Get → toJSON → default 재조회 또는 custom callback → 공용 값 정책 → scalar/container 출력을 처리한다. Key와 string 값은 quote 함수 하나, byte 값은 native Base64 primitive 하나를 쓴다. Object.keys 결과와 array length를 각 container 진입 때 확정하고, 현재 ancestor 목록 하나로 두 container의 cycle을 검사한다. Graph 복제·pre-pass·fallback은 없다. Custom replacer도 **같은 writer**에서 실행할 수 있으므로 stringifyWithReplacer를 유지할 필요가 없다.

후보의 boxed slot 판정은 Node native `util.types`를 사용했다. 이 보고서는 Node 후보 비용 판정이며, portable/browser의 boxed 판정 구현과 WebGL replacement를 완료했다는 뜻이 아니다. Production 채택 때는 같은 네 boxed 규칙과 cross-realm/proxy 의미를 portable 구현에서도 검증해야 한다. 비용 조건이 실패했으므로 그 production 작업으로 진행하지 않았다.

**기존 의미를 보존하는 한, 값 순회만 바꿔서는 Buffer.toJSON 숫자 배열이 사라지지 않는다.** 이 후보도 기본 toJSON을 먼저 호출한다. Buffer 4K에서 baseline/후보 모두 호출 1회, JSON 길이 5475를 확인했다. JSON.stringify를 예외로 바꿔도 default/custom/schema writer 검사는 통과하여 숨은 stringify fallback이 없음을 확인했다. [양방향 경로 검사](raw/check-writer-path.json), [검사 코드](tools/check-writer-path.cjs).

현재 허용되는 다음 입력 때문에 bytes를 toJSON보다 먼저 처리하는 변경은 동치가 아니다. [원본 결과](raw/observations.json).

- Buffer의 custom toJSON이 byte 1을 2로 바꾸면 현재 출력은 `Ag==`다. 그 hook을 건너뛰면 `AQ==`로 달라진다. Hook이 throw하면 현재는 실패하므로 무조건 생략하면 허용 집합도 달라진다.
- **기본 Buffer.prototype.toJSON 자체도 관찰 가능하다.** 1-byte Buffer의 length getter가 읽힐 때마다 byte를 증가시키는 허용 입력에서, 현재 기본 hook은 length를 4번 읽고 출력 `BA==`를 만든다. 기본 hook의 함수 identity만 확인하여 생략해도 이 출력은 보존되지 않는다.
- Custom replacer는 기본 toJSON의 wrapper/data 배열을 실제로 받을 수 있다. 이를 처음부터 byte string으로 바꾸면 callback 인자가 달라진다.

따라서 기본 hook을 생략하려면 hook identity, callback 관찰, length/byte metadata 및 prototype 영향에 관한 새 예외 판정이 필요하다. 또는 기본 hook의 접근·Array length 실패 의미를 별도로 재구현해야 한다. 이번 후보에는 어느 것도 넣지 않았다. 이 예외들을 숨긴 34-rule 무배열 writer를 제안하지 않는다. 기존 n4json2의 graph 복제 + fallback 설계도 반복하지 않았다.

## 비용 비교

### L4 source

CPU는 **active sample 비중**, 할당은 startup+warmup+active의 회수된 객체까지 포함하는 표본 bytes를 warmup+active 실제 message 수로 나눈 값이다. 두 범위는 다르다. 각 subtree를 더하면 중복 계산한다. Native frame과 중간 caller는 inlining으로 생략될 수 있다. [before 집계](raw/before-summary.json), [writer 집계](raw/writer-summary.json), [정규화 결과](raw/final-comparison.json).

| L4 source / 지표 | f723033a5a | single writer | raw |
|---|---:|---:|---|
| send-saturation 4K / encodePayload CPU | 50.77% | 52.14% | [before](raw/before-L4-send-saturation-4096/), [writer](raw/writer-L4-send-saturation-4096/) |
| 같은 cell / serializer CPU | 41.47% | 45.61% | 같은 raw |
| 같은 cell / Buffer.toJSON CPU | 15.93% | 22.72% | 같은 raw |
| 같은 cell / Base64 encode union CPU | 23.01% | 5.61% | 같은 raw; inlining·분모 차이를 native Base64 가속으로 해석하지 않음 |
| 같은 cell / JSON→UTF-8→binding Message까지의 encode caller CPU | 60.63% | 63.66% | 같은 raw의 encodeProductionTypedChannelPayload |
| 같은 cell / encodePayload heap | 43.33 KiB/message | 38.37 KiB/message | 같은 raw |
| 같은 cell / **UTF-8 소비까지 포함한 encode caller heap** | **43.67 KiB/message** | **44.15 KiB/message** | 같은 raw |
| 같은 cell / 전체 source heap | 47.52 KiB/message | 47.62 KiB/message | 같은 raw |
| 같은 cell / Buffer.toJSON heap | 32.10 KiB/message | 32.02 KiB/message | 같은 raw |
| request-serial 1K / encodePayload CPU | 8.10% | 9.88% | [before](raw/before-L4-request-serial-1024/), [writer](raw/writer-L4-request-serial-1024/) |
| 같은 cell / serializer CPU | 4.61% | 5.01% | 같은 raw |
| 같은 cell / Buffer.toJSON CPU | 1.96% | 2.05% | 같은 raw |
| 같은 cell / encodePayload heap | 11.69 KiB/message | 10.84 KiB/message | 같은 raw |
| 같은 cell / Buffer.toJSON heap | 8.16 KiB/message | 8.08 KiB/message | 같은 raw |
| 같은 cell / 전체 source heap | 28.33 KiB/message | 28.28 KiB/message | 같은 raw |

Request-serial의 encode caller allocation은 inlining 때문에 내부 encodePayload보다 작게 attribution되는 현상이 있어 caller 숫자로 판정하지 않았다. 전체 source heap은 같으며 poller·reply decode 등 다른 작업을 포함한다. CPU 비중의 증가를 절대 CPU 시간의 증가나 throughput 저하라고 판정하지 않는다.

4K의 encodePayload만 보면 약 5 KiB가 줄어 보인다. 후보는 Base64 string을 다른 JSON 조각과 결합한 문자열을 반환하고, 이후 UTF-8 소비에서 문자열을 실제로 읽어야 한다. 비용 범위를 그 소비까지 확장하면 차이가 없어지며 소폭 증가한다. 이 경계 이동은 아래 micro에서도 확인된다.

| counted cell | warmup+active messages: before → writer | active source completed = target received: before → writer | raw |
|---|---:|---:|---|
| send-saturation 4K | 55,648 → 74,366 | 32,936 → 41,382 | [수량·오류·종료 검증](raw/validation-summary.json) |
| request-serial 1K | 14,714 → 17,691 | 9,224 → 11,302 | 같은 raw |

모든 counted/warm L4 run은 errors=0, active submitted=completed=target received, source/target exit 0이다. Warmup 수신은 target의 `allPhaseMessages`로 확인했다. 이 수는 할당 분모 확인용이며 속도 개선 배수로 사용하지 않는다.

### Micro: 반환 string과 실제 UTF-8 소비

첫 micro는 반환값의 `.length`만 소비했다. 이 조건의 4K 전체 V8 heap은 **42.98 → 38.14 KiB/message**로 줄었다. 이 숫자만으로 채택하면 후속 문자열 소비 비용을 빠뜨린다. [최초 micro](raw/micro-4096-before-counted/), [writer](raw/micro-4096-writer-counted/).

아래 micro는 양쪽 모두 `Buffer.from(encoded,'utf8')`까지 수행한다. 0/1K/4K는 `{body: Buffer}`다. Nested DTO는 1K Buffer, 작은 Uint8Array, object/array, 64-bit bigint, null/boolean과 escape·비ASCII text를 포함한다. Bytes가 없는 같은 계열 DTO도 따로 비교했다. 각 항목은 discarded warm process 뒤 최초 counted와 두 확인 run의 3개 표본이다. [코드](tools/micro-utf8.cjs), [전체 per-run 원본·집계](raw/micro-summary.json), [최종 집계](raw/final-comparison.json).

| UTF-8 소비 shape | heap B/message 중앙값: before → writer | before 범위 / writer 범위 | raw 셀 prefix |
|---|---:|---|---|
| 0 bytes | 354 → 847 | 351–357 / 847–851 | `raw/micro-utf8-0-{before,writer}-{counted,repeat2,repeat3}/` |
| 1K | 11,371 → 11,922 | 11,351–11,396 / 11,876–11,947 | `raw/micro-utf8-1024-…/` |
| 4K | 44,156 → 44,718 | 44,154–44,298 / 44,714–44,804 | `raw/micro-utf8-4096-…/` |
| nested DTO + bytes | 13,332 → 17,454 | 13,270–13,387 / 17,417–17,501 | `raw/micro-utf8-nested-…/` |
| nested DTO, bytes 없음 | 605 → 4,292 | 603–618 / 4,282–4,293 | `raw/micro-utf8-dto-…/` |

**모든 shape에서 실제 소비를 포함한 heap 표본은 후보가 더 컸다.** 후보는 Object.keys 배열, ancestor 배열과 property closure, 부분 JSON 문자열을 직접 만들며, 기존 Buffer 숫자 배열도 남는다. 4K의 약 32 KiB 임시 숫자 배열 제거와 같은 구조적 개선은 없었다. External UTF-8 byte 저장소가 이 heap 표본에 전부 포함된다고 주장하지 않는다. 양쪽 모두 최종 UTF-8 저장소는 생성한다.

CPU는 process.cpuUsage의 active CPU us/message로 별도 기록했다. 같은 profiler를 켠 3-run 중앙값과 G5 spread는 다음과 같다. **전부 spread 5%를 초과하므로 CPU 속도 판정은 모두 판정 불가다.** 원본 CPU 값과 모든 raw 경로는 [최종 집계](raw/final-comparison.json)에 있다. 추가 반복으로 통과한 run만 고르지 않았다.

| shape | CPU us/message 중앙값: before → writer | G5 spread: before / writer | 판정 |
|---|---:|---:|---|
| 0 bytes | 3.055 → 2.104 | 64.89% / 50.85% | 판정 불가 |
| 1K | 15.464 → 13.384 | 21.21% / 18.97% | 판정 불가 |
| 4K | 38.059 → 31.891 | 66.50% / 120.24% | 판정 불가 |
| nested DTO + bytes | 19.190 → 27.885 | 138.45% / 79.78% | 판정 불가 |
| nested DTO, bytes 없음 | 7.988 → 8.994 | 56.00% / 98.70% | 판정 불가 |

Housekeeping CPU에서 다른 작업과 함께 실행한 shape profile이므로 이런 변동에서 절대 성능 결론을 내리지 않는다. 전용 lock이나 다른 CPU를 획득하여 재측정하지 않았다. 이번 채택 근거는 CPU speedup이 없다는 단정이 아니라, **일관된 할당 개선이 없고 CPU 개선도 입증되지 않았다는 것**이다.

## .NET 및 다른 언어와의 비교

.NET은 지정한 `d4json` commit **8915257322**를 직접 읽었다. 이 primary의 기존 codec와 혼동하지 않았다. [지정 commit 원문](raw/dotnet-d4json-codec.cs).

| 비용 / 소유 | .NET d4json | 현재 Node / 직접 writer |
|---|---|---|
| serialize 진입 | `ZLinkFrameworkJsonPayloadCodec.cs:37-53`의 SerializePart가 pooled IBufferWriter + Utf8JsonWriter 하나에 JsonSerializer.Serialize를 호출 | `S:37` 또는 후보 property 순회에서 JS string 생성 |
| bytes 표현 | [ByteArrayConverter.Write](https://source.dot.net/System.Text.Json/System/Text/Json/Serialization/Converters/Value/ByteArrayConverter.cs.html)가 WriteBase64StringValue에 bytes 전달 | A:10과 후보 :30 모두 borrowed native view → Base64 JS string |
| Base64 출력 | [Utf8JsonWriter](https://source.dot.net/System.Text.Json/System/Text/Json/Writer/Utf8JsonWriter.WriteValues.Bytes.cs.html)는 bytes span을 output span에 Base64로 쓰고 quote를 추가 | Base64 string과 JSON text 표현이 남고, 소비자가 UTF-8 변환 수행 |
| 임시 숫자 배열 | JS Buffer.toJSON 같은 사전 hook/숫자 배열이 없음 | 의미 보존 후보도 Buffer.toJSON 숫자 배열을 유지 |
| 최종 소유 저장소 | d4json `:53`의 Message.From에 written span 전달. Pooled buffer 확장·최종 Message 저장소 비용은 존재 | 최종 UTF-8 저장소와 binding Message 생성 비용 존재 |

.NET은 UTF-8 output에 bytes를 직접 쓰는 구조이며, JS getter/toJSON/replacer 관찰 순서를 모방할 필요가 없다. 따라서 .NET writer의 비용을 Node의 문자열 writer가 그대로 얻는다고 볼 수 없다. 이번에 .NET timing을 측정하지 않았다. 공식 STJ source 원본은 [converter](raw/dotnet-ByteArrayConverter.html), [writer](raw/dotnet-Utf8JsonWriter-Bytes.html)에 보존했다.

교차언어 확인: Java/Kotlin은 공용 JVM `ZLinkJsonMessageSerializer.java:76`의 mapper.writeValueAsBytes를 사용한다. C++은 `common/include/zlink/json_profile.hpp:260-273,518-519`에서 bytes를 Base64 string으로 써서 공통 wire를 만든다. 네 구현의 Base64 표현 계약을 바꾸는 제안이 아니다. 언어별 동적 값 종류의 accepted set이 서로 동일하다고 주장하지 않는다.

소유 계층: **Framework JSON codec와 platform byte primitive**. Core/binding/transport/execution gate 결정을 옮기지 않는다.

Spec 조항: `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md` **§5**, `01-execution/05-payload-ownership-and-codec.ko.md` **§2/§5**.

교차언어 대조 결과: **bytes의 padded RFC 4648 Base64 wire 표현 유지**, .NET의 UTF-8 direct output과 Node string output 비용 차이 확인. Runtime 교차언어 변경 없음.

변경 분류: **B 후보에 대한 조사** — 기존 의미를 유지하는 구현 비용 개선 검토. Production 변경은 **0건**. Bytes-first hook 생략으로 의미가 달라지는 안은 이번 작업의 계약 보존 조건에 맞지 않는다.

## 검증과 최종 권고

- Scratch corpus **171 pass / 0 fail**: primitive 허용·거부, bigint 경계, boxed/custom conversion, Map/Set, 다른 typed array/realm, sparse/inherited array, getter/proxy 순서, key mutation, cycle/shared subtree, custom replacer, schema, empty·large·nested 값 포함. 정상/비정상 값을 함께 비교했다. 0/1/2/3/127/128/1K/4K/64K bytes, offset view, 200K text와 모든 UTF-16 code unit을 포함한다. [결과](raw/corpus.json), [log](raw/corpus.log).
- JSON.stringify 금지 상태에서도 단일 writer default/custom/schema 검사를 통과했다. 반대로 같은 byte field의 toJSON 호출 수는 양쪽 모두 1회로 남아 배열이 제거되지 않았음을 확인했다. [결과](raw/check-writer-path.json).
- 이 검사는 전체 JavaScript 값 공간에 대한 동치 증명이나 Node 20/browser 최종 gate를 대신하지 않는다. 비용 조건이 실패했으므로 production용 broad contract test 추가·Node format/lint/typecheck/framework suite·WebGL 재생성 단계로 진행하지 않았다. Scratch baseline 공식 build만 완료했다.
- **수정 전/후 규칙 수: 35 → 34**(현재 의미를 보존하며 Buffer hook을 그대로 실행하는 Node single writer 기준). Serializer owner는 1 → 1. Callback 경로도 같은 writer다. Buffer hook을 생략하는 새 예외 규칙은 추가하지 않았다.
- 리팩토링 점검: 불필요한 Buffer 숫자 배열과 string 소비 경계의 비용 이동 **2항목 확인**, production 수정 **0**, 별도 이슈 이관 **0**. 새 cache/권위 상태/queue/lock/timer는 없다. 후보의 per-call ancestor 목록은 native recursion의 cycle 검사를 대신하지만 채택하지 않았다.

**권고: f723033a5a의 단일 JSON.stringify + replacer 구조를 유지한다.** 같은 의미의 직접 writer는 가능한 후보이나, 현재 실험에서는 byte 임시 배열을 없애지 못하고 최종 소비까지의 할당이 늘었다. CPU 개선도 확정하지 못했다. 사용자가 지정한 “규칙 수 이하이면서 비용 감소” 조건의 비용 부분을 통과하지 못했으므로 보고서에서 종료한다.

바꾼 repository 파일·local commit·남은 test 실패는 없다. Report, tools, raw와 scratch build는 검토할 수 있도록 보존했다. 이번 세션이 시작한 측정·build process는 종료했다.
