# .NET TestHost message-flow tag 조사 결과

## 결론

보고된 실패의 원인은 `.NET` Activity tag 이름이나 TestHost listener가 아니다.
실패한 `dotnet connector -> Node stream server` 단계의 Node `LoggerProvider` callback이
telemetry attribute 이름 대신 structured log 본문의 축약 key를 조회한다.

- `framework/languages/node/cross-language/node_dotnet_smoke.js:657-660`은
  `record.attributes.packet`, `flow`, `origin`을 읽는다.
- Node runtime은 `framework/languages/node/packages/framework/src/runtime/diagnostics/message-flow.ts:386-410`에서
  provider attribute를 `packet_name`, `flow_id`, `flow_origin`으로 내보낸다.
- `packet`, `flow`, `origin`은 같은 파일 `424-449`의 `zlink flow:` 본문에만 쓰는
  축약 key다.

따라서 네 줄의 `packet=<null> flow=<null> origin=<null>`은 `.NET` listener 출력이
아니다. `node_dotnet_smoke.js:654-665`가 만든 Node provider 출력이다. 실제 실패
stage는 `node_dotnet_smoke.js:649-728`이며, `.NET` listener를 사용하는 직전
`Browser TypeScript connector -> dotnet stream server` stage는 순서상 이미 통과했다
(`node_dotnet_smoke.js:42-43,49-53`).

## Spec 및 .NET 구현 대조

Message flow tracing spec의 §3.2
(`framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md:142-163`)는
`packet_name`과 함께 존재하는 `flow_id`, `flow_origin`을 Activity/telemetry attribute
이름으로 정한다. 같은 문서 §3.2의 structured log 대체 표기
(`171-178`)만 `packet`, `flow`, `origin`을 사용한다. Flow correlation spec §3
(`framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md:57-66`)은
`flow_id` 형식과 `flow_origin` 값 집합을 정한다.

`.NET` 양쪽은 이 계약과 일치한다.

- Runtime emission:
  `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkTelemetry.cs:28-48`
  (`packet_name`, `flow_id`, `flow_origin`)
- TestHost capture:
  `framework/languages/dotnet/cross-language/Zlink.Framework.TestHost/TestHostMessageFlowListener.cs:29-46`
  (같은 세 이름을 `Activity.GetTagItem`으로 조회)
- Listener registration/level:
  `.NET` listener가 실제로 필요한 stream-raw 구성은
  `TestHostScenarioConfigurator.cs:475-488`에서 listener를 등록하고 diagnostics를
  `Normal`로 켠다. `Normal`은 spec §4
  (`03-message-flow-tracing.ko.md:187-198`)에서 주요 message-flow 단계를 기록하며,
  `Detailed`는 크기와 시간만 추가한다. 이번 세 attribute의 이름이나 포함 여부를
  바꾸지 않는다. 실패한 반대 방향의 stream-client 구성(`498-506`)에는 `.NET`
  listener가 없고, Node server의 provider가 flow를 관찰한다.

## 변경

Runtime이나 TestHost에는 고칠 불일치가 없어 production 코드를 변경하지 않았다.
`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/MessageFlowTracerTests.cs:60-112`의
기존 `MessageFlowActivityUsesNormalizedSpecAttributes` test에 `packet_name`, `flow_id`,
`flow_origin`의 정확한 이름과 값을 추가했다. 이 test는 `.NET` Activity emission이
spec 이름에서 벗어나면 실패한다.

Node smoke의 직접 수정 후보는 `node_dotnet_smoke.js:660`에서 각각
`fields.packet_name`, `fields.flow_id`, `fields.flow_origin`을 읽는 것이다. 그러나 작업
지시는 수정 owner를 `framework/languages/dotnet/**`로 한정하므로 이 파일은 변경하지
않았다.

## 검증

- `git diff --check`
  - 결과: 성공
- `cd framework/languages/node && node --test test/contract/message-flow.test.js`
  - 결과: 성공, 40 passed / 0 failed
  - `MFLOW-003/005 standard logger provider receives the structured record`가
    `record.attributes.packet_name`을 직접 확인한다.
- 지정된 환경과 `flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet test ... --filter
  'FullyQualifiedName~MessageFlowTracerTests.MessageFlowActivityUsesNormalizedSpecAttributes'
  --no-restore`
  - 결과: test 시작 전 중단. 다른 전체 `.NET` unit gate가 같은 lock과 dotnet test
    process를 1시간 이상 점유하고 있었다. 해당 사용자 process는 종료하지 않았다.
- `framework/languages/node/cross-language/run_cross_language_smoke.sh`
  - 결과: 새 실행을 시작하지 못했다. 이 script도 `.NET` TestHost를 빌드·실행하므로
    위 공유 gate와 충돌시키지 않았다.
  - 기존 `zlink-work/c016/logs/c-e2e-2-07-node-cross.log:32-42`는 Node provider의 잘못된
    key 조회로 같은 실패를 이미 보존한다.

## BLOCKERS

1. 요구한 smoke 2회 green은 `.NET` 파일만 수정해서 만들 수 없다. 직접 원인은
   `framework/languages/node/cross-language/node_dotnet_smoke.js:660`에 있다.
2. focused `.NET` test 및 smoke 재실행은 `/tmp/zlink-dotnet-gate.lock`을 점유한 기존
   장기 실행 gate 때문에 완료하지 못했다.
3. Supervisor가 Node cross-language harness 수정을 승인하면 위 세 조회 key를 spec
   attribute 이름으로 바꾼 뒤 focused `.NET` test와 smoke를 2회 실행해야 한다.
