# #1675 Node 리뷰 (감독자, 2026-10-15) — 4480c2eb84..9de2a4a3fa, 33 files +2013/-582, 전부 읽음
판정: 승인 조건부(N1·N2 수정 뒤). 스펙 gap 0.
대조한 조항: 08 E1–E5·§3 전환 수·§4.1 I0–I4·§4.2·§4.3 W1–W3·§6, 01 §10·§11·§15, 05 payload 소유, bindings/doc/spec/message-ownership 15(Node receive part는 JS-owned Buffer — retainPayload 무복사 근거), 06-obs/04 §3(flow_id = 소문자 UUIDv7 36B — counter 방식 허용).
로그 직접 확인: final2-test 212 블록 fail 0, m6a 44/44, m6c 252/252, format OK, m6b 131/132(#117 "Spot does not participate in subscription channel 'events'" — base-m6b.log에서 같은 실패).
N1 (CONTRIBUTING §5): test/contract/node-dispatch-turn.test.js 'an idle connected node waits for its next management deadline' — 실제 200ms sleep 뒤 timer 수를 센다. node:test mock timers 등 주입 시계로 결정적으로 바꾼다.
N2 (AGENTS §3 중복 금지): isPromiseLike가 runtime/admission.ts:217, runtime/channels/channel-dispatch-pipeline.ts:468, runtime/streams/stream-session-runtime.ts:1514 세 곳. 하나로 모은다(기존 소유자 재사용 또는 한 곳으로 옮김).
넘긴 항목(이 PR 밖): ZLinkEncodedPayload from()/data() 전체 복사(공개 API — 결정 필요), pump wake Promise 약 10/request-serial msg. 스펙 S2 확정 뒤 Node flow 생성 조건(runtime/host/index.ts:3164 mode !== 'off')을 Normal 이상으로 맞춘다.
- 2026-10-15 main merge 18d78e3dad 충돌 없음. Node 2732/2733, 실패 1은 main에서도 간헐(#1819), m6a 44/44.

## 2차 리뷰 (감독자, 2026-10-15) — framework-node/1675-per-message-cost-2, 564ec281f1..ecd88da442 (9파일 +253/-183)
판정: 승인. 스펙 gap 0.
- 2f9c5f4ca7 S2: messageFlowTracingEnabled 한 곳(message-flow.ts), host/index.ts:3170·tracer가 사용. 시험 2건은 Normal로(04-flow-correlation.ko.md:68-69,:91 — flow field 검증은 tracing 중에만), 단언 불변.
- 02dfc65e22 pump: dispatchRecord/dispatchPermittedRecord/finishPermittedRecord 세 사본 → 하나, permit 반환 4→2곳. 동기 throw·비동기 reject·성공 경로 모두 이전과 같은 순서(terminal 뒤 permit 반환, 실패 시 terminal 없이 반환+보고). releaseApplicationJobPermitTurn(undefined) 허용 확인.
- 3c1540e30a m6a: frame()=allocUnsafe는 prefix·본문 전부를 씀(reply :155-160 확인). 텍스트 검증 round-trip → fatal TextDecoder(ignoreBOM)+NUL 거부, 쓰기 쪽 isWellFormed — 같은 입력 집합. backend 중복 decoder 삭제, decodeMultipartApplicationPayloadView가 version·body 길이·이름 byte 일치·payload 길이·end 검사(이전과 같은 거부 집합). reply tail은 frame view(JS 소유 Buffer라 수명 안전).
- 28ef820303 correlation ID: 프로세스 64bit random prefix + counter, 32 hex. 04-flow-correlation §3(1..64 byte opaque ASCII, 같은 lifecycle의 대기 중 request 사이 중복 금지) 충족.
- 2bc745465b 수신 payload 무복사: 내부 encodedPayloadFromOwned(JS 소유 receive Buffer — bindings message-ownership 15), 공개 from()/data() 복사 의미 유지.
결정: R-a 승인(공개 동작 불변). P-a 승인(조건: backend-contract 24 20회, node-dispatch-turn W1–W3, 전체 211파일). E-a/E-b/E-c는 공개 API·패키지 구조 결정이라 사용자에게 올림.
- 52980c72ce R-a 승인: closure 4개를 RequestAdmission 상태 객체로, confirm/fail/observe 의미·순서 동일. 3895827e6c P-a 승인: application worker 전용 drain loop(beginDrain/finally draining-=1, hasResidue 재표시, 공유 yield 예산 recordsSinceYield/yieldIfNeeded 유지), drainDomain은 infrastructure 전용(lifecycle claim budget 유지). 시험 2712·m6a 44, yield 20/20 단독·200/200 병렬, dispatch-turn 4/4(codex 보고, 로그 m1675e-report).
- zlink 승인 #1834 (13:39).
- 2026-10-16 사용자 결정: E-a 채택(E-b·E-c 기각). Node 3차로 측정 뒤 투입.
- 2026-10-16 E-a 감독자 리뷰(spec 6e15063809, impl 8eaf61efde): 반려 1건.
  - 결함: 수신 deserialize가 view()를 decoder에 넘기는데 @msgpack/msgpack 3.1.3 Decoder.decodeBinary(dist.cjs/Decoder.cjs:655)는 bin을 `this.bytes.subarray`로 돌려준다 → application 값이 payload(binding 수신 Buffer)를 별칭. classic fanout처럼 같은 message를 여러 handler가 deserialize하면 한 handler의 bin 수정이 다른 handler 입력을 바꾼다. 전에는 data() 복사로 격리됐다. envelope protobuf serializer의 options.decode(사용자 제공, protobufjs bytes는 slice view)도 같다. dynamic protobuf(dynamic-value-wire.ts:130 Buffer.from 복사)는 안전.
  - 관찰(결함 아님): msgpack encode()는 새 Encoder의 encodeSharedRef → 초기 버퍼(2KB) subarray. fromOwned가 그 ArrayBuffer 전체를 보관 — 송신 뒤 native 변환까지만 사는지 확인 요청.
  - 스펙: view 소비자는 반환 application 값이 view를 참조하지 않게 한다는 규칙 필요. 02 문서 머리말("동작 의미는 공통 스펙이 소유")과의 관계 — 05 링크.
- 2026-10-16 E-a 재리뷰 승인: impl a53d48f4d1(msgpack·envelope 수신은 data() 복사로 격리, dynamic protobuf만 view — dynamic-value-wire.ts:130 Buffer.from 복사 확인), spec 2fc54ad08a(codec 반환값의 view 참조 금지 + 05 §2 링크, 앵커 확인). 수정 전 실패 2건(red.log), npm test 2716/2716·M6A 44/44·codec 31/31 로그 확인. deserialize 호출 3곳 모두 ZLinkEncodedPayload 전달 확인(actor-manager 시험 수정은 시험 fake가 Message를 넘기던 오용). 할당: send 5→4, request 12→11. msgpack 2KB backing은 pending request 동안 유지 — 조건부 후속 후보(channel-envelope.ts:150-200 request part만 정확한 길이).
