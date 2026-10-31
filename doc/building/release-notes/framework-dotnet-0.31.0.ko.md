[English](./framework-dotnet-0.31.0.md) | [한국어](./framework-dotnet-0.31.0.ko.md)

# ZLink .NET Framework 0.31.0 릴리스 노트

Framework 0.31.0은 .NET binding 1.21.0과 Core 1.21.0을 사용합니다.

## 결함 수정

- Session handler가 대기 중에도 STREAM 연결 해제 후 binding 정리가 진행됩니다. (#3185, #3189)
- Join이 완료되기 전에 도착한 payload를 보류하고, relocation 뒤 순서대로 재생합니다. (#2483, #3054)
- Actor Join의 generation과 Store membership 실패 결과를 실제 activation 및 Store 결과에 맞춥니다. (#2738, #2873, #2903)
- Channel handler를 FIFO 순서로 시작하고 앞 handler의 완료를 기다리지 않습니다. (#2758, #2858)
- RouteMesh peer 준비 상태를 상대 노드의 ADMIT 수신으로 확정합니다. (#2790, #2807)
- 종료 중인 Spot의 Join·Close 결과와 거부된 Join 뒤 host drain 처리를 수정했습니다. (#2672, #2675, #2682, #2683, #2688)
- Session bind fence를 제출된 ActorRef의 Store row에서 정하고, 관련 service-wire 명령을 schema 생성 codec으로 처리합니다. (#2507, #2514, #2517, #2588)
- Actor 응답의 STREAM frame part 수와 Error payload를 계약에 맞추고, Join 확정 뒤 target lifecycle 실패를 보고합니다. (#2513, #2426)
- liveness probe 제출과 physical RID 정리의 결과 처리를 수정하고, 제출 전 분류되지 않은 실패는 원래 원인과 함께 InternalFailure로 전달합니다. (#2369, #2380, #2381)
- WebSocket connector가 OS proxy 설정을 따르지 않고 지정 endpoint에 직접 연결합니다. (#2534, #2648)
- relocation rollback admission 및 transport replay, deferred Join claim 처리 순서를 수정했습니다. (#2408, #2227, #2261)
- service command가 ingress에서 확인한 source lifecycle을 사용하고, control-send 실패만으로 route admission을 종료하지 않습니다. (#2914, #2923, #2784)
- 여러 프레임워크 런타임의 owner 경계를 정리해 bind registration, request admission, actor activation·completion, host 종료와 relocation 처리 결과를 일관되게 합니다. (#2140, #2242, #2260, #2267, #2270, #2275, #2276, #2280, #2284, #2285)
- 연결이 준비되기 전에 들어온 remote request를 admission owner 하나가 판정합니다. 준비된 연결에서는 대기 구독 없이 바로 제출합니다. (#1710, #3259)
- `GetOrCreate`가 새 생성 capacity를 보기 전에 기존 authority를 확인합니다. capacity가 모두 쓰였을 때 이미 있는 User Spot을 다시 얻는 요청이 실패하던 문제를 수정했습니다. (#2799, #3242)

## 성능 개선

- request-serial과 direct-request 경로에서 node turn·closure·빈 List·reply header 복사와 불필요한 forwarding await를 줄였습니다. (#1674, #2241, #2604)
- client send/request 성공 경로의 header 재생성·body 복사·문자열 비용과 request 취소 관찰자 할당을 줄였습니다. (#1674, #2363, #2371)
- envelope decode에서 전체 frame 복사를 없애고, header 검증을 할당 없는 범위 검사로 처리합니다. (#1674, #2255, #2905, #2912)
- receive batch, header 재사용, worker pool 및 idle worker 정지 동작을 개선했습니다. (#1640, #1643, #1648, #2164, #2171, #2178, #2186, #2206, #2273, #2279)
- 논리 request의 단일 deadline을 owner별 인덱스로 관리해 request별 timer를 제거하고, request 제출과 physical completion의 단일 소유권을 보존합니다. (#2758, #2900, #3142)
- runtime metrics와 Channel receive·handler 단계의 진단 계기를 추가·정리했습니다. (#2189, #2263, #2412, #2799, #3171, #3180)
- `AddLogging` 기본 등록에 provider가 없으면 message-flow 기록을 만들지 않습니다. 사용자가 등록한 `ILoggerFactory`는 그대로 사용합니다. `Zlink.Framework.AspNetCore`가 `Microsoft.Extensions.Logging` 8.0.0을 직접 참조합니다. (#2758, #3246)

## 알려진 제한

- 시험 코드의 private 멤버 접근을 공개 경로로 옮기는 작업을 Node·Java·.NET에서 9차까지 진행했습니다. 남은 범위는 다음 릴리스에서 이어 갑니다. 제품 동작에는 영향이 없습니다. (#2309)
- Windows에서 mailbox signaler가 AF_UNIX datagram 실패 뒤 stream으로 전환하면 socket마다 임시 경로를 만들고 지우는 데 약 2.5ms가 듭니다. 수정은 다음 Core 릴리스에 포함됩니다. (#3053)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.31.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.31.0" />
```

릴리스 태그는 [`framework-dotnet/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.31.0)입니다.
