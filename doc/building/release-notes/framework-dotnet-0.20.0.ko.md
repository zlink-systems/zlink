[English](./framework-dotnet-0.20.0.md) | [한국어](./framework-dotnet-0.20.0.ko.md)

# ZLink .NET Framework 0.20.0 릴리스 노트

Framework 0.20.0은 binding 1.2.2와 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- durable-authority-v1 activation recovery pointer가 `root`, `activationId`, `ownerGeneration`, `replayCursor`, `inboxSequence`의 5개 field로 통일됩니다. `replayCursor`는 `inboxSequence`를 넘을 수 없으며 이전 .NET 전용 ZLIS 형식은 사용하지 않습니다. (#759)
- stream connector는 연결·끊김·상태·push·error handler와 request callback의 완료를 기다리지 않고 반환합니다. `Closed` 뒤 끊김 handler가 실행되는 순서는 유지됩니다. (#594)
- 원격 Actor 생성의 완료 terminal은 target이 소유합니다. requester는 응답이 유실되거나 미완료·예외 응답을 받은 경우 operation identity로 creation terminal을 재조회하며, application 생성 실행 예외는 typed `Failed` terminal로 기록됩니다. Location Store에는 `creation-terminal\0{hex(rid)}\0{generation}\0{hex(operationId)}` opaque record 하나가 사용되고 value는 `creation-operation-terminal-v1` schema bytes입니다. (#774)
- 최초 owner lease claim은 heartbeat의 첫 회에 수행됩니다. transport 실패는 host를 종료시키지 않으며, lease가 없으면 §5 차단 상태로 startup을 완료한 뒤 renew interval마다 claim합니다. `Conflict`와 `GenerationExhausted`는 typed startup error입니다. (#790)
- durable authority의 owner lease가 `Missing`이면 최초 claim은 새 owner로 인계하고, `Found`이면 다른 owner로 판단합니다. .NET과 Java의 저장소 판정이 C++·Node의 공통 동작과 일치합니다. (#682)
- service-wire-v1은 schema dialect 규칙과 생성된 정적 codec을 사용합니다. sha256은 `u8 + 32` bytes, ZLIA는 presence byte, `operation-id-or-zero`는 명시적 schema case, reference는 `u16`으로 정정되며 .NET codec은 세 언어 codec과 같은 byte를 생성·해석합니다. (#729, #736, #737)
- 원격 Actor 생성은 Entry Spot type 등록과 무관하게 Framework Entry Spot ID를 게시하고, 예약·commit·abort는 authority row의 `pendingCreation` 하나로 판정합니다. target이 완료와 terminal을 소유하며 actor-create-terminal conditional union은 canonical `u16` body length를 사용합니다. (#561, #550, #763)

이 릴리스는 1.0 이전 계약입니다. 기존 계약으로의 전환 절차는 없습니다.

## 공통 변경

- service-wire schema를 operation IR로 낮추고 C++·C#·Java·TypeScript 정적 codec을 생성하는 경로를 추가했습니다. 생성 manifest, fixture catalog, validator와 네 언어 conformance가 같은 schema를 사용합니다. (#736, #780)
- tutorial·samples를 저장소 없이 배포 zip에서 빌드·실행할 수 있도록 정리했습니다. .NET sample runner에서 Python proxy를 제거하고, 릴리스에 네 언어 tutorial·samples zip 8개를 첨부하는 자산 경로를 추가했습니다. (#655, #639, #673)
- 네 언어 tutorial에 Instance Spot `MatchQueue` 예제를 추가하고, C++·.NET·Java·Kotlin·Node/TypeScript의 snippet marker를 정렬했습니다. (#666)
- tutorial·samples 소스를 언어별 포매터 출력에 맞추고 `scripts/format/format.sh --check`를 공통 진입점으로 정했습니다. (#761)
- ZoneWorld 샘플의 shared browser client가 `npm run prepare:browser` 하나로 의존성과 Chromium을 스스로 준비합니다. `run_sample`의 browser smoke는 별도 설치 없이 돕니다. (#815)
- ZoneWorld `run_sample`의 browser smoke(crash lane)를 baseline 시나리오 뒤로 옮겼습니다. zone-node-2를 종료한 뒤 zone 0개 replacement로 바꾼 상태에서 baseline ZW-A3·B1~B6이 돌아 실패했습니다. (#819)
- ZoneWorld `run_sample`의 browser smoke는 G4·B8처럼 child topology에서 돕니다. baseline 시나리오와 shared client의 Playwright 테스트가 모두 초기 토폴로지를 전제하므로 같은 토폴로지를 공유할 수 없었습니다. (#824)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.20.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.20.0" />
```

릴리스 태그는 [`framework-dotnet/v0.20.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.20.0)입니다.
