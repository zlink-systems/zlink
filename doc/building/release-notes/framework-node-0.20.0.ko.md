[English](./framework-node-0.20.0.md) | [한국어](./framework-node-0.20.0.ko.md)

# ZLink Node.js Framework 0.20.0 릴리스 노트

Framework 0.20.0은 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- `receiveTimeoutMs`가 ROUTER socket의 수신 timeout으로 적용됩니다. 동작하지 않던 공개 `mailboxMessageBudget`·`mailboxByteBudget` 옵션은 제거됩니다. (#606)
- durable-authority-v1 activation recovery pointer가 `root`, `activationId`, `ownerGeneration`, `replayCursor`, `inboxSequence`의 5개 field로 통일됩니다. `replayCursor`는 `inboxSequence`를 넘을 수 없으며 이전 4-field 형식은 사용하지 않습니다. (#759)
- stream connector는 연결·끊김·상태·push·error handler와 request callback의 완료를 기다리지 않고 반환합니다. `Closed` 뒤 끊김 handler가 실행되는 순서는 유지됩니다. (#594)
- 원격 Actor 생성의 완료 terminal은 target이 소유합니다. requester는 응답이 유실되거나 미완료·예외 응답을 받은 경우 operation identity로 creation terminal을 재조회하며, application 생성 실행 예외는 typed `Failed` terminal로 기록됩니다. Location Store에는 `creation-terminal\0{hex(rid)}\0{generation}\0{hex(operationId)}` opaque record 하나가 사용되고 value는 `creation-operation-terminal-v1` schema bytes입니다. (#774)
- 최초 owner lease claim은 heartbeat의 첫 회에 수행됩니다. transport 실패는 host를 종료시키지 않으며, lease가 없으면 §5 차단 상태로 startup을 완료한 뒤 renew interval마다 claim합니다. `Conflict`와 `GenerationExhausted`는 typed startup error입니다. (#790)
- durable authority의 owner lease가 `Missing`이면 최초 claim은 새 owner로 인계하고, `Found`이면 다른 owner로 판단합니다. Node는 기존 저장소 판정을 유지하면서 네 언어의 공통 규칙에 맞춥니다. (#682)
- service-wire-v1은 schema dialect 규칙과 생성된 정적 codec을 사용합니다. sha256은 `u8 + 32` bytes, ZLIA는 presence byte, `operation-id-or-zero`는 명시적 schema case, reference는 `u16`으로 정정되며 TypeScript codec은 세 언어 codec과 같은 byte를 생성·해석합니다. (#729, #736, #737)
- 원격 Actor 생성은 Entry Spot type 등록과 무관하게 Framework Entry Spot ID를 게시하고, 예약·commit·abort는 authority row의 `pendingCreation` 하나로 판정합니다. target이 완료와 terminal을 소유하며 actor-create-terminal conditional union은 canonical `u16` body length를 사용합니다. (#561, #550, #763)
- incomplete Actor Join fence는 legacy 입력으로 처리되지 않고 `ProtocolError`가 됩니다. fence field가 일부만 있으면 검증을 중단하고 거부합니다. (#781)

이 릴리스는 1.0 이전 계약입니다. 기존 계약으로의 전환 절차는 없습니다.

## 공통 변경

- service-wire schema를 operation IR로 낮추고 C++·C#·Java·TypeScript 정적 codec을 생성하는 경로를 추가했습니다. 생성 manifest, fixture catalog, validator와 네 언어 conformance가 같은 schema를 사용합니다. (#736, #780)
- quickstart·tutorial·samples를 저장소 없이 빌드·실행할 수 있도록 정리했습니다. 받는 방법은 언어별 examples 저장소 `git clone https://github.com/zlink-systems/zlink-node-examples`이며(태그 `v0.20.0`이 이 릴리스와 같은 내용), zip 배포는 없앴습니다. (#655, #639, #673, #831)
- 네 언어 tutorial에 Instance Spot `MatchQueue` 예제를 추가하고, C++·.NET·Java·Kotlin·Node/TypeScript의 snippet marker를 정렬했습니다. (#666)
- tutorial·samples 소스를 언어별 포매터 출력에 맞추고 `scripts/format/format.sh --check`를 공통 진입점으로 정했습니다. (#761)
- ZoneWorld 샘플의 shared browser client가 `npm run prepare:browser` 하나로 의존성과 Chromium을 스스로 준비하고, runner는 browser lane을 transition client를 arm하기 전에 끝냅니다(ops 세션의 application idle 30초 안에 준비가 끝나지 않으면 ZW-C3가 실패했습니다). (#815)
- shared browser client의 `prepare:browser`가 링크된 workspace 패키지(`stream-connector` 등)의 빌드 산출물이 없으면 workspace `npm ci`·build까지 준비합니다. 새 worktree의 browser 모드가 vite 단계에서 막히지 않습니다. (#822)

## 설치

```bash
npm install @zlink-systems/framework@0.20.0 @zlink-systems/http-client@0.20.0
```

릴리스 태그는 [`framework-node/v0.20.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.20.0)입니다.
