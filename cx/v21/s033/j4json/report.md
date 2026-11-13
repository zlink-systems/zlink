# Java L4 typed JSON — payload 계약 확인

**Step 1a에서 공통 perf DTO와 L4 bench DTO의 차이를 확인하여 중단한다.** 공통 perf는 identity와 여러 metadata field 및 `payload: Text`를 정의한다. Java L4는 `byte[] body` 하나다. 반면 gRPC bench의 protobuf 스키마는 실제로 `bytes body` 하나를 정의한다. 두 문서의 적용 범위를 구분해야 하며, 이 보고서는 gRPC bench에 공통 perf DTO를 임의로 적용하거나 DTO를 교체하지 않는다. L4에서 어느 payload 계약을 사용할지는 PM과 실장님이 결정할 사항이다.

## Step 1a — bench payload

기준 repository는 `/root/worktree/zlink-v21-s033/j4json`, branch는 `perf/4354-java-typed-json`, HEAD와 origin/main은 `df42a4df4098d543be8dc0b84510cf6d961576d8`이다. 시작과 종료 시 tracked/untracked 변경이 없다. 다른 작업의 코드 확인은 지정 commit에 대한 `git show`만 사용했다. 근거 원문은 [spec-check.txt](raw/spec-check.txt), [bench-dtos.txt](raw/bench-dtos.txt), [task-context.txt](raw/task-context.txt), [repository-state.txt](raw/repository-state.txt)에 보존했다.

### 공통 perf 스펙

| 위치 | 원문과 판정 |
|---|---|
| `framework/doc/framework/common/perf/README.ko.md:5-8` | 공통 공개 호출 흐름의 비교 조건을 정의하고, 표준 scenario·payload matrix·CLI·결과 형식을 소유한다. gRPC 비교 bench와 문서 범위가 같다거나 L4 DTO가 같다고 자동으로 해석할 수는 없다. |
| 같은 파일 `:896-897` | “Framework message는 기본 typed JSON serializer로 전달한다.” payload JSON 표현은 §15에 고정한다. |
| 같은 파일 `:903-904,911` | request/initial send는 `PerfEchoRequest`, typed reply는 `PerfEchoReply`이며 field type과 JSON은 §15.2가 소유한다. |
| 같은 파일 `:1106-1120` | `Identity`에 runId/cellId/resetSeq/phase가 있고, request에는 clientId/sequence/correlationId/sentTicks/clockDomainId/returnSpotId/returnChannel과 payload가 있다. `:1120` 원문은 `payload: Text // logical bytes의 canonical padded Base64`다. |
| 같은 파일 `:1122-1128` | reply에도 identity 및 metadata field가 있다. `:1128` 원문은 `payload: Text // 받은 Base64를 그대로 echo`다. |
| 같은 파일 `:1160-1163` | logical byte pattern은 `(31*i + 17*floor(i/251) + 29) mod 256`이다. “Base64 변환은 application field 표현이고 message 전체의 수동 codec이 아니다.” 같은 typed JSON serializer를 사용한다. |

따라서 **단일 `byte[] body` DTO는 이 공통 perf DTO가 아니다.** property 이름, field 수, application field type과 Base64 변환의 위치가 다르다. 공통 perf는 application이 Base64 텍스트를 제공하지만 bytes DTO는 typed serializer가 매번 bytes를 Base64로 변환해야 한다. 이 차이만으로 현재 손실의 원인이나 기여율을 판정하지는 않는다.

### gRPC benchmark 계약과 L4 지시

| 위치 | 원문과 판정 |
|---|---|
| `9d9901eb2c:framework/bench/grpc/proto/bench.proto:19-20` | `message BenchPayload { bytes body = 1; }` — 단일 bytes body를 명시한다. Java L4의 논리 field 구성은 이 스키마와 일치한다. |
| `framework/bench/grpc/README.ko.md:76-80` | 크기는 protobuf `bytes body`의 전체 크기이고 앞 29 bytes는 측정 header다. |
| 같은 파일 `:156-162` | “gRPC와 ZLink framework는 같은 protobuf DTO를 사용한다.” envelope part와 protobuf BenchPayload part를 규정한다. |
| 같은 파일 `:474-485` | 메시지마다 BenchPayload 생성과 protobuf 직렬화/역직렬화를 규정하며 Java framework는 generated 타입과 `ZLinkProtobufCodec`을 사용한다. |
| `/root/jobs/fx/cx/v21/s033/issues-4351-4357.md:18-21` | #4354는 기본 typed JSON serializer와 메시지별 codec 등록 금지를 지시한다. 새 DTO field 목록이나 공통 perf DTO로의 교체는 정의하지 않는다. |
| `/root/jobs/fx/cx/v21/s033/ladder-steps.md:5-10,20-29` | L4에서 typed codec을 추가하며 L0~L3는 protobuf body를 유지한다. Java L4의 BenchTypedPayload record와 기본 JSON 사용을 기록한다. 이는 현재 구현 기록이며 공통 perf의 DTO 정의를 바꾸는 스펙 조항은 아니다. |

L4의 JSON 사용 자체는 #4354의 명시적 지시다. **gRPC protobuf 스키마와 공통 perf JSON DTO는 서로 다른 계약**이며, 기존 protobuf body를 단일 bytes DTO로 옮긴 현재 L4가 공통 perf의 표준 payload와 같다는 근거는 확인되지 않았다. 스펙 불일치 시 중단하라는 이번 지시에 따라 다음 단계로 진행하지 않는다.

### 네 언어 DTO 대조

아래 경로는 모두 표에 적힌 commit의 파일이다. 다른 measuring worktree나 build output은 열거나 수정하지 않았다.

| 언어·commit | DTO 근거 `file:line` | 실제 구성 |
|---|---|---|
| Java `9d9901eb2c` | `framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/BenchTypedPayload.java:5-6` | `public record BenchTypedPayload(byte[] body) {}`. 기본 serializer 인스턴스는 client `RawStack.java:479-481`, target `ZLinkFrameworkBenchServer.java:346-348`. |
| C++ `276b84f248` | `framework/bench/grpc/cpp/ladder/ladder_payload.hpp:13-18,30-34` | `typed_payload_t`의 유일한 저장 field는 `std::vector<std::byte> bytes`. dto_traits는 JSON property `body` 하나에 매핑한다. `:21-26`에서 bytes 생성과 header stamp, `:38-41`에서 기본 registry를 사용한다. |
| Node `ae8a5178b5` | `framework/bench/grpc/node/shared/framework-bench-contract.js:17-20`; `client/main.js:261,307`; `zlink-framework-server/main.js:113` | BenchPayload는 body 하나이며 L4에서는 `{ body: payload }`를 넘긴다. `shared/raw-wire.js:119-138`은 codec 인자 없이 production encode를 호출하고 `:141-154`는 body를 Base64로 decode한다. |
| .NET `4cbe0b5316` | `framework/bench/grpc/dotnet/Shared/BenchPayloads.cs:33`; `Client/Program.cs:648-653` | `public sealed record BenchTypedPayload(byte[] Body);`이며 body bytes로 생성한다. |

네 언어 모두 단일 body라는 점에서 gRPC BenchPayload의 논리 구성과 맞는다. 어느 언어도 공통 perf의 PerfEchoRequest/Reply field 집합을 사용하는 구성은 아니다. Java만 DTO를 바꿀 근거는 없다.

## Step 1b — typed JSON wire 계약

**byte sequence → padded Base64 string은 스펙에 명시된 wire 계약이다.**

`framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:134-153`의 `framework-json-v1`은 모든 언어의 기본 typed application payload에 적용된다(`:136-138`). `:153` 원문:

> Byte sequence는 padding을 포함한 RFC 4648 base64로 표현한다.

따라서 bytes DTO를 유지한다면 Base64를 numeric array나 raw bytes로 바꾸는 수정은 허용되지 않는다. `:144`는 property 순서와 의미 없는 whitespace가 의미 없다고 하지만, 이번 작업의 더 엄격한 byte-identical 출력 요구도 유지한다. 공통 perf의 `payload: Text`가 Base64라는 사실과 generic typed serializer가 byte sequence를 Base64로 encode하는 계약은 구별한다. 전자는 application DTO 정의이고 후자는 serializer의 wire 표현이다.

## Step 2·3 — 중단 범위

| 항목 | 결과 |
|---|---|
| 원인 및 production `file:line` | 조사하지 않았다. Step 1a 중단 조건을 적용했다. 중간 String/char[]/StringBuilder, tree model, reflection, mapper 생성, buffer copy 또는 UTF-8 재인코딩을 손실 원인으로 단정하지 않는다. |
| CPU·allocation profile | 실행하지 않았다. raw 아래 파일은 계약 확인 원문이며 profile 결과가 아니다. L3/L4 send-saturation 4K·request-serial 1K profile 및 allocation/copy count는 미완료다. |
| scratch/build/bench/test | 생성·실행하지 않았다. 모든 셸 명령과 자식 프로세스는 `taskset -c 6-7,14-15`를 상속했다. measure.lock은 사용하지 않았다. |
| production 수정 제안 | 보류한다. DTO 및 측정 조건에 관한 PM·실장님 결정 뒤 serializer 소유 모듈에서 조사해야 한다. wire/spec/public API 변경을 제안하거나 승인으로 간주하지 않는다. |
| 소유 계층·스펙 | bytes의 JSON 표현은 Framework typed serializer가 소유한다. 근거는 message-model §5 `:136-153`. application perf DTO는 perf §12/§15.2가 소유한다. |
| 교차언어 결과 | 단일 body DTO는 네 언어 공통이다. runtime encode 구현 비교는 중단하여 수행하지 않았다. |
| 변경 분류·규칙 수 | production 수정 없음으로 A/B/C/D 분류를 하지 않는다. 수정된 결정 위치 0 → 0, 추가 상태·flag·queue·lock 없음. |
| 리팩토링 점검 | runtime 호출 경로 감사는 미실시다. 결함 수를 0으로 판정하지 않는다. 고친 항목 없음, 계약 확인 이후로 넘긴 항목은 profile·encode 원인 조사다. |
| 변경 파일·commit | 이 report.md와 raw의 계약 근거 파일만 생성했다. repository 변경과 local commit은 없다. 외부 경로의 보고서를 commit하려고 repository에 사본을 추가하지 않았다. |
| 남은 실패 | test 실패는 관측하지 않았다(test 미실행). 기존 L4 손실 수치를 이 작업에서 재현하거나 개선했다고 판정하지 않는다. |

PM·실장님이 결정할 사항은 L4가 gRPC 단일 bytes body를 유지하는 실험인지, 공통 perf의 텍스트 payload DTO를 따라야 하는지다. 그 결정 전에는 DTO, wire, 측정 조건 또는 production serializer를 수정하지 않는다.
