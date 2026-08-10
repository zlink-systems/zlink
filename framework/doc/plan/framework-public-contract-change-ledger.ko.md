# Framework public contract 변경 ledger

이 문서는 Framework internals 통합과 POSDDD 리팩터링을 수행하면서 public contract 또는
언어별 exact interface를 변경해야 하는 경우, 유지보수자가 변경 이유와 호환성 영향을 한
곳에서 판단할 수 있도록 실행 근거를 기록한다. 임시 작업 기록이며 정식 spec·internals·guide는
이 문서를 참조하지 않는다.

## 변경 권한과 적용 기준

사용자는 2026-08-10에 구현을 끝까지 진행하는 데 필요한 public contract와 exact interface
변경을 승인했다. 구현 차이를 감추기 위한 API는 추가하지 않는다. 현재 공개 표면으로 계약을
지킬 수 없다는 production call path와 cross-language 증거가 있을 때만 변경하며, 같은
checkpoint에서 다음 항목을 함께 처리한다.

- 공통 contract와 언어별 exact interface의 현재 목표 상태
- C++/.NET/JVM/Node public surface parity
- 기존 호출자의 source·binary 호환성 및 필요한 migration
- contract test, package gate와 sample 검증
- 변경을 포함하는 commit과 pushed SHA

## 변경 기록

내부 queue, relocation protocol 구현과 white-box test 변경은 기존 공개 계약을 구현하는
작업이므로 이 표에 넣지 않는다. `IN-PROGRESS` 항목은 아직 한 package에서 같은 계약을
적용하는 작업이 남았음을 뜻한다.

| ID | 상태 | 변경 전·후 | 변경 이유와 근거 | 호환성·migration | spec·interface·test | pushed SHA |
|---|---|---|---|---|---|---|
| CT-01 | APPLIED | 이전에는 codec 등록 content-type의 유효 범위와 대소문자·공백 정규화가 정해져 있지 않았고 언어별로 trim, case-insensitive lookup, exact lookup이 달랐다. 변경 뒤에는 Framework service wire/custom registry에서 parameter가 없는 ASCII `type/subtype`만 받는다. 등록 시 바깥 SP·HTAB을 제거하고 ASCII lowercase로 바꾸며, wire에는 이 canonical form만 허용한다. HTTP response parameter는 HTTP client가 parse한 뒤 bare media type만 registry에 전달한다. | 같은 extension을 등록해도 언어별 receive 결과가 달랐고 Node·JVM custom codec은 exact case, .NET은 case-insensitive lookup을 사용했다. Wire 값을 startup table과 바로 비교하도록 고정하면 hot path 정규화와 allocation 없이 같은 `ProtocolError` 결과를 낸다. HTTP의 정상 `charset` parameter까지 거부하지 않도록 transport 경계를 분리했다. | Public method signature와 package ABI는 바뀌지 않는다. 기존에 uppercase, parameter, 내부 공백 또는 non-ASCII content-type을 등록한 custom extension은 parameter 없는 lowercase media type으로 바꿔야 한다. Framework가 송신하는 값은 등록 시 canonicalize된다. HTTP response의 정상 parameter 사용은 유지된다. | 공통 spec §9와 C++/.NET/Java/Kotlin/Node exact interface를 갱신했다. `codec-selection-v1.json`과 언어별 fixture consumer test로 검증했다. | Node `562f19494b`, C++ `cb247017e8`, .NET `ac67839a54`, JVM `607d8bfcd0` |
| CT-02 | IN-PROGRESS | 송신 serializer 선택은 runtime instance를 검사하거나 둘 이상의 match를 ambiguity로 처리하던 방식에서, 호출 지점이 보존한 declared message type을 selector에 전달하고 나중에 등록한 match를 우선하는 방식으로 바뀐다. Node는 `addSerializer(contentType, serializer, canSerialize)`와 `ZLinkMessage.from(value, declaredType)` overload를 추가한다. Java는 serializer에 declared `Class`를 전달하는 default overload와 `ZLinkMessage.of(value, declaredType)`·`declaredType()`을 추가한다. Kotlin은 reified `messageOf(value)`와 명시적 `messageOf(value, declaredType)`을 제공한다. C++는 compile-time payload descriptor, .NET은 기존 declared `Type` predicate를 사용한다. | Runtime subtype에 따라 같은 호출의 content-type이 달라지면 수신 계약과 cache key가 불안정해진다. TypeScript와 Java의 `Object` 경계는 호출 지점의 static type을 자동으로 남기지 않으므로, base type과 runtime subtype이 다를 때 declared type을 보존할 표면이 필요하다. 네 구현은 같은 1,024-entry non-evicting send-type cache와 JSON no-match fallback을 사용한다. | C++/.NET의 public signature는 유지한다. Node의 기존 2-argument 등록과 Java serializer의 기존 `serialize(value)`는 source·binary 호환을 유지하는 fallback이다. Serializer 객체에 비정식 `canSerialize(value)` member를 넣던 Node 코드는 세 번째 registrar argument인 `(declaredType) => boolean`으로 옮겨야 한다. Base/interface 의미를 보존해야 하는 Node·Java 호출은 declared-type message overload를 사용한다. 기존 Kotlin `messageOf(value)` source call은 그대로 유효하며 reified type을 추가로 보존한다. | 공통 spec codec 선택 절과 C++/.NET/Java/Kotlin/Node exact interface·public snapshot을 갱신했다. `codec-selection-v1.json`의 declared-base/runtime-derived, later-match, JSON fallback과 cache saturation scenario를 네 언어 consumer test로 검증한다. .NET Framework server registry는 적용됐지만 HTTP client registry의 later-registration 우선과 1,024-entry cache 정렬이 POSDDD checkpoint에 남아 있다. | Node `562f19494b`, C++ `cb247017e8`, JVM `607d8bfcd0`; .NET server `ac67839a54`, HTTP client 대기 |
| CT-03 | APPLIED | 받은 message의 lazy decode가 호출 횟수나 target type마다 payload를 다시 역직렬화할 수 있던 동작에서, 첫 typed decode의 값 또는 실패를 message가 단일 outcome으로 보관하는 동작으로 바뀐다. .NET·Node·Java/Kotlin·C++ 모두 같은 message의 후속 decode에서 codec을 다시 호출하지 않는다. .NET과 Node는 첫 값을 재사용하고, Java/Kotlin과 C++는 다른 target type을 type mismatch 또는 `protocol_error`로 끝낸다. 네 구현 모두 첫 실패를 보관한다. | Accepted payload는 Framework가 성공·실패와 관계없이 한 번만 역직렬화해야 한다. 반복 accessor가 codec을 다시 호출하면 malformed payload에서 비용과 side effect가 반복되고 payload ownership budget도 어긴다. | Public signature와 wire 형식은 바뀌지 않는다. 같은 message를 여러 번 decode해 서로 다른 mutable 객체를 기대했거나, 첫 실패 뒤 다른 target type으로 다시 해석하던 코드는 첫 outcome을 공유하도록 바뀐다. 호출자가 독립 객체나 다른 표현이 필요하면 첫 결과를 명시적으로 변환하거나 복사해야 한다. .NET의 반복 `ReadOnlyMemory<byte>` view와 명시적 `byte[]` copy는 typed slot을 사용하지 않는다. | 공통 message spec과 네 언어 exact interface를 갱신했다. `payload-ownership-v1.json`의 최대 역직렬화 횟수 1을 직접 사용하며, 언어별 test가 성공·실패, 반복·동시 접근과 다른 target type에서도 codec 호출이 한 번인지 검증한다. | Node `4c237ffc84`, C++ `cb247017e8`, .NET `ac67839a54`, JVM `607d8bfcd0` |
