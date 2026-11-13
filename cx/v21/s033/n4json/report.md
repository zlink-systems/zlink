# Node L4 typed JSON — 원인과 production 수정 결과

**Node 기본 JSON serializer의 spread·binary string 준비 비용을 native Base64 변환으로 교체했다.** 원인은 browser용 Base64 primitive를 Node에서도 사용한 데 있었다. 구현 전에 원인·수정안을 기록했고 [원본 보고서](raw/report-before-implementation.md)를 보존했다. Buffer.toJSON의 버려지는 숫자 배열도 존재하지만 CPU 주원인은 아니다. wire는 이미 padded RFC 4648 Base64 문자열이다. DTO와 wire를 유지했으며 새 회귀 검사 19개와 관련 Framework 검사 131개가 모두 통과했다.

## 근거와 범위

- Primary: `perf/4354-node-typed-json`, HEAD/base `df42a4df4098d543be8dc0b84510cf6d961576d8`; 시작 시 변경 없음. Runtime 두 파일과 회귀 test 한 파일을 수정했다.
- Scratch: `/root/worktree/zlink-v21-s033/n4json-scratch`, `ae8a5178b5` git archive, 자체 npm ci와 공식 `npm run build`. 다른 job worktree나 build output을 사용하지 않았다. 해당 commit과 main의 JSON profile·Node adapter·channel payload 경로는 동일하다. 전체 Node tree의 Actor 차이 때문에 다른 build를 대체하지 않았다.
- Node 22.23.3. 모든 실행 process는 `taskset -c 6-7,14-15`를 상속했다. measure.lock 획득, GitHub, push, PR, 다른 AI 도구, internal-docs.sh 실행 없음.
- 지정한 `lad-node-impl4c/report.md`는 없다. `lad-node-impl4/report.md`와 지정 commit의 실제 L4 호출을 확인했다. [호출 비교표](call-shape.md), [원본 코드와 line](raw/baseline-source.txt), [ladder 코드](raw/ladder-source.txt).
- 최초 profile 시도 `raw/before-*`는 CJS codec bundle 누락으로 source가 ready 이전에 종료한 무효 자료다. TypeScript compile 뒤 공식 build로 번들을 만들고 `raw/before2-*`를 수집했다. 첫 실패 raw도 보존했다. [원인 log](raw/before-L3-send-saturation-4096/source.log), [공식 scratch build](raw/build-full-scratch-before.log).

## 원인: default production path

| 위치 (primary, df42a4df40) | 확인한 동작 |
|---|---|
| `packages/framework/src/runtime/channels/channel-envelope.ts:498-508,535-557` | codec 인자가 없는 typed object는 `stringifyFrameworkJsonV1(value, schema)`로 전달한다. envelope encode와 reply encode가 같은 owner를 사용한다. |
| `packages/framework/src/runtime/messaging/framework-json-v1.ts:1-4` | 공용 stream-wire JSON 구현을 그대로 재수출한다. Node용 byte 변환 primitive 선택이 없다. |
| `packages/stream-wire/src/internal/framework-json-v1.ts:23,45-65` | JSON.stringify가 Buffer.toJSON을 호출한 뒤 replacer가 `this[key]`의 원본 Buffer를 찾아 Base64로 바꾼다. 임시 숫자 배열은 wire에 포함되지 않고 버려진다. |
| 같은 파일 `:377,394-399` | chunk는 8192 bytes. `String.fromCharCode(...bytes.subarray(...))`로 byte iterable을 N개 인자로 펼쳐 binary string을 만들고 btoa를 호출한다. 1K와 4K 모두 한 chunk다. 이 구간의 source 구조는 선형이며 이번 결과로 초선형 복잡도를 주장하지 않는다. |
| Node 22 `node:buffer:1154-1161` | Buffer.toJSON은 N개 숫자 배열과 `{type,data}` 객체를 만든다. [로컬 runtime 원문](raw/node-base64-builtins.txt). |
| `channel-envelope.ts:343-356,434-440,901-902`; 공용 profile `:79-90,309-318` | JSON body를 UTF-8 text로 바꾸고 scanner와 native JSON.parse를 사용한다. L4는 schema·reviver를 제공하지 않아 bytes decode와 per-key reviver가 없다. escape 없는 Base64는 scanner의 native delimiter 검색을 사용한다. |
| `shared/raw-wire.js:131,137,146,148,154` (ae8a5178b5) | outbound Buffer.from(string,'utf8')는 최초 UTF-8 byte 생성이다. inbound Buffer.from(encoded)는 J-byte 전체 복사다. payload.body를 Buffer.from(base64)로 바꾸는 것은 schema 없는 bench의 byte 복원이며 production parse 이후다. bench DTO나 이 코드는 바꾸지 않는다. |

위 표의 packages 경로는 `framework/languages/node/` 아래다. production channel encode 자체에는 payload byte Buffer 복사와 UTF-8 인코딩이 없다. String 반환 이후 binding/bench가 byte를 생성한다.

## CPU·할당 profile

각 run은 `--cpu-prof`와 Inspector HeapProfiler sampling 16 KiB를 함께 사용했다. minor/major GC로 회수된 객체를 포함한다. [수집기 검증](raw/preload-smoke/alloc-start-287944.json), [수집 코드](tools/allocation-preload.cjs), [driver](tools/profile-cells.py), [집계 코드](tools/summarize-profiles.py).

3초 warmup + 4초 active. 전체 process lifetime은 8.96–9.73초, source/target exit 0. 네 cell 모두 active submitted=completed=received, errors=0이다. 각 cell의 `scheduling.txt`는 start/mid-active 모든 thread의 affinity와 policy를 기록한다. profiling 조건의 처리량을 기존 측정치와 비교하지 않는다. raw results에 들어 있는 throughput은 판정에 사용하지 않았다.

CPU 비중은 active 구간의 sample 개수 기준이다. allocation은 startup+warmup+active 동안의 회수된 객체까지 포함한 표본이며, 해당 function subtree bytes를 실제 처리 message 수로 나눈 근삿값이다. inlining에 따라 allocation stack의 중간 frame이 생략되므로 서로 다른 subtree 값을 합산하지 않는다. 전체 집계: [JSON](raw/before2-summary.json), [text](raw/before2-summary.txt).

| cell | role | Base64 subtree CPU | Buffer.toJSON CPU | parse JSON CPU | encodePayload sampled allocation/message | raw |
|---|---|---:|---:|---:|---:|---|
| L3-request-serial-1024 | source | 0.00% | 0.00% | 0.00% | 0.5 KiB | [9,890 messages / profile](raw/before2-L3-request-serial-1024/summary-344083.json) |
| L3-request-serial-1024 | target | 0.00% | 0.00% | 0.00% | 0.7 KiB | [9,890 messages / profile](raw/before2-L3-request-serial-1024/summary-344082.json) |
| L3-send-saturation-4096 | source | 0.00% | 0.00% | 0.00% | 0.1 KiB | [126,173 messages / profile](raw/before2-L3-send-saturation-4096/summary-341990.json) |
| L3-send-saturation-4096 | target | 0.00% | 0.00% | 0.00% | 0.0 KiB | [126,173 messages / profile](raw/before2-L3-send-saturation-4096/summary-341989.json) |
| L4-request-serial-1024 | source | 5.97% | 0.78% | 0.67% | 86.8 KiB | [4,864 messages / profile](raw/before2-L4-request-serial-1024/summary-348021.json) |
| L4-request-serial-1024 | target | 6.64% | 0.80% | 1.08% | 87.7 KiB | [4,864 messages / profile](raw/before2-L4-request-serial-1024/summary-348020.json) |
| L4-send-saturation-4096 | target | 0.00% | 0.00% | 2.45% | 0.0 KiB | [15,599 messages / profile](raw/before2-L4-send-saturation-4096/summary-345913.json) |
| L4-send-saturation-4096 | source | 66.21% | 3.58% | 0.00% | 332.9 KiB | [15,599 messages / profile](raw/before2-L4-send-saturation-4096/summary-345914.json) |

send-saturation 4K source에서는 Base64 subtree가 66.21%, 전체 stringify가 75.86%다. Buffer.toJSON은 3.58%, target JSON parse는 2.45%다. request-serial 1K source는 completion poller 비중이 크지만 Base64가 5.97%, reply를 encode하는 target은 6.64%다. 두 방향 모두 production encode의 같은 변환이 보인다. L3에서는 해당 Base64·toJSON stack이 없다.

## 메시지당 복사·할당 회계

N은 logical body bytes, B=`4*ceil(N/3)`, J=`B+11` (`{"body":"..."}`)이다. 4K는 B=5464, J=5475다. 아래는 source에 명시된 변환·객체와 직접 계측한 호출 수다. V8 내부 객체 수는 source-visible 수와 구분하며 allocation sampling으로 확인한다.

| 구간 | schema 없는 1 body field의 메시지당 비용 | 원문 |
|---|---|---|
| Buffer.toJSON | Buffer이면 Array(N) 1개 + wrapper object 1개, 숫자 원소 N개 저장. Uint8Array는 0개. 그 배열의 숫자에 대한 replacer 호출은 0회. | `node:buffer:1154-1161` |
| Base64 준비 | N≤8192에서 subarray view 1개, N개 인자 목록을 만드는 spread 1회, N-char binary string 1개. view 자체는 byte 복사가 아니다. iterator·spread 내부 객체 개수는 V8 구현에 따른다. | 공용 profile `:394-399` |
| JSON body encode | btoa 1회 → B-char Base64 string, JSON.stringify 1회 → J-char JSON string. replacer는 root/body 두 번이다. production Buffer.from(payload) 복사와 UTF-8 인코딩은 0회. | profile `:13-28,45-65`, channel `:556-557` |
| Envelope 고정 비용 | 반환 parts array 1개, no-registry header 경로의 writer 2개와 최종 header Buffer 1개. payload 크기와 무관하다. | channel `:512-532,618-659` |
| JSON body decode | J-char UTF-8 decode string 1개, scanner 1개와 root path array 1개, nonempty object의 Set 1개, property/value slice, native parse가 만드는 DTO object와 Base64 string. source slice의 실제 저장소 공유는 V8에 따른다. byte Buffer 전체 복사는 0회, reviver 0회, schema bytes decode 0회. | channel `:353-356,434-440`; profile `:79-90,266,309-318` |
| bytes schema가 있을 때만 | validator의 결과 object/entries, atob binary string 1개, 새 Uint8Array(N) 1개와 N번 charCodeAt. 같은 bytes의 추가 Uint8Array 복사는 없다. 이번 bench는 이 schema 경로를 사용하지 않는다. | profile `:137-144,168-192,402-406` |
| L4 bench 경계 | outbound 최초 UTF-8 생성 J bytes + binding Message.from의 독립 사본; inbound target에서 추가 Buffer.from(encoded) J-byte 복사 1회; schema 없는 body의 Base64→bytes N-byte 생성 1회. 기존 service framing 복사는 L3에도 있다. | ae8a5178b5 raw-wire `:131,137,146-154`; binding Message.from 계약 |

[16개 byte size/type 단일 호출 계측](raw/before-counts-expanded-deterministic-counts.json)은 4096-byte Buffer에서 toJSON=1/N=4096, fromCharCode 인자=4096, btoa=1, replacer=2/숫자 callback=0, production Buffer.from byte-copy/UTF-8=0을 확인했다. Uint8Array는 toJSON=0이며 나머지는 같다. 모든 원문 JSON과 schema 유무별 decoded 값도 확인했다.

## 결정 소유 표와 교차언어 확인

| 결정 / 소유 spec | Node 현재 판정 위치 | Java/Kotlin 현재 위치 | .NET 현재 위치 | C++ 현재 위치 | 수정 후 owner |
|---|---|---|---|---|---|
| 기본 typed JSON 선택 / message-model §5 | channel-envelope `:498-508` | ZLinkJsonMessageSerializer `:54-76` (Kotlin도 같은 JVM 구현) | ZLinkFrameworkJsonPayloadCodec `:26-33` | contracts/codecs/serializer.hpp `:345-375` | 기존 Framework codec 선택 유지 |
| byte sequence → padded RFC 4648 Base64 / message-model `05-message-model.ko.md:153` | 공용 framework-json-v1 `:63-65,394-399`; Node adapter `:4` | mapper.writeValueAsBytes `ZLinkJsonMessageSerializer:76`, canonical bytes 검증 `ZLinkFrameworkJsonProfile:352-375` | System.Text.Json 경로 `ZLinkFrameworkJsonPayloadCodec:26-33` | common/include/zlink/json_profile.hpp `:258-273,518-519` | 타입·wire 판정은 공용 JSON profile 한 곳; Node adapter가 native primitive 제공 |
| bytes 복원·canonical check / message-model §5 및 payload ownership §2/§5 | 공용 profile `:137-144,386-406` | ZLinkFrameworkJsonProfile `:352-375` | ZLinkFrameworkJsonPayloadCodec `:10-23`의 JSON validator와 typed serializer | json_profile.hpp `:275-300,805-807` | 기존 decoder 유지 |

네 구현 모두 byte sequence를 Base64 JSON string으로 표현한다. Node의 binary string/spread는 wire 계약이 요구하는 단계가 아니며, 다른 언어의 codec primitive 선택을 Framework 호출자가 재구현할 이유가 없다. 교차언어 실행 suite는 이번 단계에서 실행하지 않았다. [근거 원문](raw/baseline-source.txt).

## 선택한 production 수정

1. 공용 `stream-wire/src/internal/framework-json-v1.ts`에서 기존 stringify 정책을 내부 stringifier factory로 묶고 byte encoder function만 받는다. 기본 portable encoder는 그대로 두며 finite number, bigint, Date/toJSON 거부, getter 처리, schema 검증, custom replacer 의미를 한 곳에 유지한다. 새 factory는 `internal/json`에만 존재하며 package root public export는 바꾸지 않는다.
2. `packages/framework/src/runtime/messaging/framework-json-v1.ts`의 기존 Node adapter가 이 factory에 `Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('base64')`를 제공한다. ArrayBuffer/offset/length overload는 view이며 N-byte 사본을 만들지 않는다. source 타입 판정은 기존 공용 bytes 분기 하나가 맡는다.
3. channel-envelope encode/reply는 기존 호출을 그대로 사용한다. decoder, content type, framing, DTO, codec 등록 API는 바꾸지 않는다. Browser용 기본 변환도 유지한다.
4. Buffer.toJSON의 임시 Array(N)는 남는다. 이를 없애려면 JSON.stringify 이전 객체 그래프 변환이나 별도 writer가 필요해 getter·toJSON·cycle 등 추가 규칙이 생긴다. 이번 수정은 profile에서 지배적인 spread/binary-string 변환을 제거하고 이 잔여 비용을 그대로 보고한다. native Base64와 JSON 출력 string 생성도 계속 필요하다.

| 대안 | 판정 |
|---|---|
| 기존 JSON 정책 + Node native byte primitive (선택) | 타입·검증 규칙을 유지하며 N-argument spread와 binary string을 제거한다. Node 경계에서 primitive를 한 번 구성하므로 message별 platform 조건·cache·새 상태가 없다. |
| DTO를 먼저 복제·정규화하거나 별도 JSON writer 작성 | Buffer.toJSON 비용까지 피할 수 있지만 property/getter·cycle·boxed primitive·custom toJSON 규칙을 다시 소유해야 한다. 기존 JSON 의미와 판정 위치를 늘리므로 채택하지 않는다. |

- 소유 계층: Framework JSON codec, Node runtime adapter. Core·binding·transport·execution gate 결정 변경 없음.
- spec: `00-foundation/05-message-model.ko.md:134-153`, `01-execution/05-payload-ownership-and-codec.ko.md:29-43,100-121`.
- 교차언어: Base64 JSON wire와 기존 JSON 검증 의미를 그대로 유지한다. Node runtime primitive만 교체한다.
- 변경 분류: **B — 기존 성능 결함**. spec/wire/public API 변경 없이 구현했다.
- 규칙 수 전/후: Node byte 표현 판정 위치 **1 → 1**, JSON 검증 owner **1 → 1**. 새 flag·queue·lock·timer·cache 없음. factory capture는 process 초기화 때만 만든다.
- 리팩토링 점검: portable byte 변환을 Node hot path에서 쓰는 항목 1건을 수정했다. control/gate 동기화는 이번 경로에 추가하지 않는다. Buffer.toJSON 잔여 할당은 위에 구분했다.

## 수정 후 profile·검증

같은 scratch bench와 CLI로 네 cell을 다시 수집했다. 수정한 production 두 TS 파일만 scratch에 옮긴 뒤 공식 npm run build로 갱신했다. bench JS와 DTO는 그대로다. source/target은 모든 cell에서 exit 0, errors 0, submitted=completed=received다. 전체 lifetime 9.10–11.27초이며 모두 20초 이내다. [driver](raw/profile-after-driver.log), [전체 집계](raw/after-summary.json).

| cell / role | Base64 active CPU sample 비중 | encodePayload allocation 표본 / message | raw |
|---|---:|---:|---|
| L4-send-saturation-4096 / source | 66.21% portable → 4.35% native | 332.9 → 43.3 KiB (약 87.0% 감소) | [전](raw/before2-L4-send-saturation-4096/summary-345914.json) · [후](raw/after-L4-send-saturation-4096/summary-567729.json) |
| L4-request-serial-1024 / source | 5.97% portable → 0.39% native | 86.8 → 11.4 KiB (약 86.8% 감소) | [전](raw/before2-L4-request-serial-1024/summary-348021.json) · [후](raw/after-L4-request-serial-1024/summary-568088.json) |
| L4-request-serial-1024 / target | 6.64% portable → 0.48% native | 87.7 → 11.3 KiB (약 87.1% 감소) | [전](raw/before2-L4-request-serial-1024/summary-348020.json) · [후](raw/after-L4-request-serial-1024/summary-568087.json) |

CPU 비중은 각 run의 sample 구성이다. 함수 실행시간이나 처리량 개선 배수로 해석하지 않는다. portable Base64 stack은 수정 후 L4에서 사라졌다. source 4K의 Buffer.toJSON은 16.71% CPU와 약 32.1 KiB/message 할당 표본으로 남는다. 이는 새로 생긴 비용이 아니라, 지배적이던 spread/binary-string 비용을 제거한 뒤 드러나는 잔여 비용이다. target decode 코드는 바꾸지 않았다. 4K target parse 비중은 2.45→6.09%이며, 이 구성 변화도 decode 성능 판정으로 사용하지 않는다.

단일 호출 [수정 전](raw/before-counts-expanded-deterministic-counts.json) · [수정 후](raw/after-deterministic-counts.json)는 16개 size/type 조합 모두 byte-identical JSON을 확인했다. 수정 후 Buffer/Uint8Array 모두 fromCharCode 인자=0, btoa=0, byte Buffer 복사=0, ArrayBuffer view=1, replacer=2다. Buffer.toJSON의 Array(N)과 wrapper object는 유지한다. schema 없는 Base64 값과 bytes schema의 원래 byte 값도 동일하다.

| 검사 | 결과 | raw |
|---|---|---|
| 새 bytes 회귀 검사, 수정 전 | 18 pass / 1 fail. portable btoa를 금지한 검사가 실제 production 경로에서 실패 | [log](raw/new-test-before-fix.log) |
| 새 bytes 회귀 검사, 수정 후 | **19 pass, 0 fail, 0 skip**. 8개 크기 × Buffer/Uint8Array, offset view, request/reply, schema 유무, source 불변, 공유 getter/replacer 의미, 잘못된 padding 거부 | [log](raw/new-test-after-fix.log) |
| 관련 Framework 검사 10 files | **131 pass, 0 fail, 0 skip**. channel-envelope-error, framework-json bytes/parser/scan/schema-generator, stream-connector-json, node-envelope-codec-cost, node-ingress-header, contract-surface, stream-wire enum contract | [log](raw/focused-framework-tests.log) |
| 공식 Node build | primary / scratch 모두 exit 0, browser bundle 포함 | [primary](raw/build-full-primary-after.log) · [scratch](raw/build-full-scratch-after.log) |
| Node format check | 전체 Node TS exit 0 | [log](raw/node-format-check.log) |
| 수정 TS lint | exit 0 | [log](raw/lint-touched.log) |
| package root public export | factory는 Framework/stream-wire root에 노출되지 않음 | [확인](raw/public-export-check.json) |
| git diff --check | exit 0 | 최종 commit 전에도 확인 |

실행 integrity·source SHA: [validation-summary.json](raw/validation-summary.json). profile의 scratch는 git archive이므로 bench metadata의 commit은 unknown이며, 각 run의 provenance.json과 progress.md가 archive commit, primary HEAD/base/dirty와 실제 JS/native binary SHA를 기록한다.

## 바꾼 파일과 남은 항목

| 파일 | 변경 |
|---|---|
| [Node adapter](/root/worktree/zlink-v21-s033/n4json/framework/languages/node/packages/framework/src/runtime/messaging/framework-json-v1.ts:5) | 기본 stringifier를 native Base64 primitive로 구성. `:8-11`에서 원본 byte 범위를 빌려 변환한다. |
| [공용 JSON policy](/root/worktree/zlink-v21-s033/n4json/framework/languages/node/packages/stream-wire/src/internal/framework-json-v1.ts:16) | 내부 factory에 byte encoder만 제공한다. 타입 판정은 `:72` 한 곳에 유지한다. default portable backend와 모든 JSON 검증 규칙을 유지한다. |
| [회귀 test](/root/worktree/zlink-v21-s033/n4json/framework/languages/node/test/contract/node-typed-json.test.js:1) | byte-identical wire와 decoded 값, offset 범위, native path 및 JSON 정책을 고정한다. |

공식 build가 자동 갱신한 Unity WebGL 생성물은 Node 수정 범위 밖이다. 이 세션이 만든 diff를 [raw](raw/generated-unity-diff.patch)에 보존하고 원래 내용으로 돌렸다. 기존 사용자 변경은 시작 때 없었으며 다른 job worktree/build를 수정하지 않았다. Raw 파일은 모두 보존한다. Scratch code/build도 profile 재현·검토용으로 보존하며 시작한 build/test/profile process는 종료했다.

남은 test 실패는 없다. Buffer.toJSON 임시 숫자 배열, Base64/JSON output string 생성, 최초 UTF-8 인코딩과 기존 bench 수신 복사는 남는다. 처리량 판정, 전체 네 언어 runtime 실행, push·PR·GitHub 작업은 하지 않았다.

소유 계층: **Framework JSON codec / Node runtime adapter**.

스펙: **message-model §5 `:153` padded RFC 4648 Base64**, payload ownership §2/§5의 복사 회계.

교차언어 대조: **네 구현의 Base64 JSON 표현 유지**. 이번 검증은 Node의 공용 fixture·portable serializer와 byte-identical 비교이며 네 runtime 동시 실행 검사는 하지 않았다.

변경 분류: **B — 기존 성능 결함**, spec/wire/public API 변경 없음.

규칙 수: **Node byte 타입 판정 위치 1 → 1** (`frameworkJsonV1Value`). 새 상태·flag·queue·lock·cache 없음. 리팩토링 점검은 hot path 변환 1건 수정, control/gate 중복 추가 0건, 별도 이슈로 넘긴 항목 0건이다. 위 잔여 할당을 제거했다고 주장하지 않는다.

로컬 commit: `6e48bdf5fbfe7252b029b405f514693a91f2a92d` (`perf/4354-node-typed-json`). 작업 tree는 clean이다. 검증한 세 source 파일의 SHA가 commit 이후에도 동일함을 확인했다. [최종 repository 상태](raw/repository-final.json).
