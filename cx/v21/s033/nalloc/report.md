# Node outgoing frame 직접 할당 결과

## writer 경로와 Message 소유권

기존 multipart application frame은 `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:2950-2957`에서 `encodeMultipartApplicationPayload` 계열의 Buffer 경로로 만들었다. 실제 호출은 instance send/request `:364-389`, node send/request `:740-838`, channel send/request `:842-872`, reply `:2682-2685`에 있다. 프레임 바이트는 `framework/languages/node/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts:277-292`의 `writeMultipartPayload`가 썼고, 이 과정의 `result.set`은 각 multipart part를 완성된 프레임으로 한 번 복사했다.

그 뒤 `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts:493-500,505-514`가 각 part를 `operation.message(...)`에 전달했다. Buffer 입력은 `node-backend-adapter-support.ts:302-318`에서 `Message.from`으로 바뀌어 전체 프레임이 native Message 저장소로 한 번 더 복사됐다. `bindings/node/src/zlink/runtime/messaging/received_operations.ts:25`는 이미 Message인 입력은 그대로 전달하고, `bindings/node/src/zlink/runtime/sockets/socket_operation_builders.ts:41-48,62-83,123-126`은 성공한 submit에서 Message를 consume한다.

`Message.allocate(size)`와 writable `data()`는 기존 Node binding API다 (`bindings/node/src/zlink/contracts/messaging/message.ts:118-123`). Binding 규칙은 성공 submit에서 ownership을 넘기고 동기 submit 실패에서는 caller가 Message를 보유하는 것이다 (`bindings/doc/spec/node/README.ko.md:574`, `bindings/doc/spec/README.ko.md:1697-1744`). 따라서 새 코드에서 allocate 뒤 encode 실패, route 불가, local snapshot, submit 실패와 durable route 대기 취소는 Framework가 닫는다. 성공 submit은 binding이 consume한다. Raw reply의 submit 실패도 caller가 소유하므로 닫는다.

## 변경과 규칙 수

`service-wire-m6a-codec.ts:233-250`에 `Message.allocate(layout.frameLength)`를 사용해 최종 native 프레임을 만들고, `data()` view에 header와 multipart body를 기록하는 내부 함수를 추가했다. 기존 `writeMultipartPayload`를 재사용해 별도 writer나 body encode 경로를 만들지 않았다. `node-raw-mesh-backend.ts:2950-2957`의 send, request, reply 호출부는 Message를 그대로 raw binding에 넘긴다. Protobuf body 생성 위치는 유지했고, protobuf 결과를 최종 multipart frame에 넣는 part 복사는 한 번 남는다.

결정을 내리던 위치는 변경 전 2곳이었다. Framework codec이 Buffer frame을 할당해 썼고 Node binding adapter가 다시 Message를 할당해 전체 frame을 복사했다. 변경 후에는 최종 저장소를 선택하는 위치가 codec 한 곳이며 binding은 Message identity를 전달한다. 판정 위치는 2곳에서 1곳으로 줄었다.

Message 정리는 `runtime/backend/runtime-message.ts:4-9`의 공통 helper로 모았다. 이 helper는 `node-backend-adapter-support.ts`, `node-raw-binding-port.ts`, `raw-service-mesh-runtime.ts`, `service-stateful-runtime.ts`, `durable-service-request.ts`에서 submit 실패와 route 취소 같은 미소유 경로를 정리한다.

## 중복 작업 점검

`service-wire-m6a-codec.ts:290`의 part별 `result.set`은 multipart part를 final frame에 넣는 유일한 복사이며 유지했다. 전체 Buffer frame을 `Message.from`으로 복사하던 단계는 제거했다. `node-backend-adapter-support.ts:302-318`의 Message 변환은 Buffer 등 기존 입력 호환 경로로 남고, 새 outgoing frame은 Message identity 경로를 탄다.

같은 node로 들어가는 요청은 `raw-service-mesh-runtime.ts:1743-1751` 또는 `service-stateful-runtime.ts:6496-6504`에서 local ingress가 보유할 Buffer snapshot을 만든다. 이 복사는 local delivery의 소유권 경계에 필요해 유지했다. `Message.close()` 검사와 반복은 `runtime-message.ts:4-9`로 합쳤고 stateful runtime과 Node adapter에 따로 있던 close loop는 제거했다. 새 queue, lock, cache, flag 또는 state는 추가하지 않았다.

## 검증

`npm test` 전체 통과 로그: `/root/jobs/fx/cx/v21/s033/nalloc/npm-test-final.log`. 마지막 M6A runtime contract suite는 51/51 통과했다. outgoing frame 계약 테스트는 `Message.from`을 실패하도록 대체한 상태에서도 encode와 실제 raw send가 성공하는지 확인한다. 추가 테스트는 allocate 뒤 명시적 close, 강제 submit 실패 뒤 close를 확인한다.

Node format 검사 통과 로그: `/root/jobs/fx/cx/v21/s033/nalloc/format-final.log`. `git diff --check` 통과 로그: `/root/jobs/fx/cx/v21/s033/nalloc/diff-check.log`. main/change 및 benchmark build 로그는 각각 `/root/jobs/fx/cx/v21/s033/nalloc/build-main.log`, `build-change.log`, `bench-build.log`에 있다.

## 1 KiB 무제한 측정

같은 배타 측정 세션에서 condition B, RUNS=5, warmup 2초, active 5초로 실행했다. `Framework µs/msg`와 `binding µs/msg`는 각 처리량 median의 역수다. `F÷B`는 Framework µs/msg ÷ binding µs/msg다. 예산 초과는 `Framework µs/msg − binding µs/msg ÷ 0.6`으로 계산했다. 음수는 예산 안쪽이다. 전체 log는 `/root/jobs/fx/cx/v21/s033/nalloc/bench-measure.log`, 셀 JSON은 `main/run*/`과 `change/run*/` 아래에 있다.

| 시나리오 | 판 | Framework µs/msg | binding µs/msg | F÷B | 예산 초과 µs/msg |
|---|---|---:|---:|---:|---:|
| send-saturation | main | 13.488 | 8.599 | 1.568 | −0.845 (예산 안쪽) |
| send-saturation | change | 20.641 | 8.579 | 2.406 | 6.342 |
| request-backpressure | main | 61.131 | 36.379 | 1.680 | 0.499 |
| request-backpressure | change | 72.742 | 35.124 | 2.071 | 14.201 |
| request-serial | main | 198.515 | 108.500 | 1.830 | 17.682 |
| request-serial | change | 215.862 | 117.110 | 1.843 | 20.679 |

각 benchmark cell에서 완료 수와 target 수신 수가 같았다. 변경판의 처리량 역수는 세 시나리오 모두 main보다 높았다. 이 측정은 direct Message frame encode 경로를 포함하며, 뒤에 추가한 실패·취소 cleanup 통합은 정상 submit 경로를 바꾸지 않는다.

## Runtime 변경 기록

소유 계층은 Framework codec이 application frame bytes와 Message storage를 소유하고, binding이 submit acceptance와 성공 consume를 소유한다.

Spec 조항은 `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md`의 Framework multipart application profile §2와 `bindings/doc/spec/node/README.ko.md`의 submit ownership 규칙이다.

교차언어 대조에서 기존 보고서의 Node body pass는 4/2이고 .NET은 3/2였다. .NET은 `framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:100`에서 protobuf body를 native Message span에 쓴다. Node는 protobuf 생성 위치를 유지하고 final multipart frame만 기존 `Message.allocate` 저장소에 기록해 Node의 추가 full-frame copy 한 회를 없앴다. 이에 따라 outgoing Node body pass는 3/2가 된다.

변경 분류는 A 계약 적응이다. 기존 binding API와 wire format을 유지한다.
