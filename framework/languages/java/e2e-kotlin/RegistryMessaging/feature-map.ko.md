# Kotlin RegistryMessaging E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md`

Kotlin 구현은 Java runtime 위의 Kotlin 사용 표면을 검증한다. Client는 framework client를 직접 들지
않고 실제 role server의 HTTP endpoint를 호출한다. framework request/send/route 호출은 Provider,
Consumer, Workflow role server endpoint 내부에서 public API로 수행한다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| RM-A1 | implemented | Redis location store 자동 연결로 discovery consumer request를 보내고, public MeshNode descriptor와 provider evidence를 검증한다. |
| RM-A2 | implemented | provider role의 manual endpoint 경로로 `api-a` 도달을 검증한다. |
| RM-A4 | implemented | Redis location store 동적 cluster에서 같은 rid provider v1/v2를 교체하고 discovery consumer가 MeshNode descriptor 제거/재등장과 instance 전환을 검증한다. |
| RM-A6 | implemented | discovery consumer가 API channel과 workflow channel을 각각 Redis location store 자동 연결로 호출하고 channel별 evidence를 확인한다. |
| RM-B1 | implemented | Redis location store 동적 cluster에서 provider B 추가 뒤 discovery consumer가 public MeshNode descriptor 2개와 두 provider routing을 확인한다. |
| RM-B2 | implemented | Redis location store 동적 cluster에서 provider B 종료 뒤 public MeshNode descriptor 제거와 survivor `api-a` routing을 검증한다. |
| RM-B3 | 전환 필요 | provider A handler-start evidence 뒤 A를 `SIGKILL`하고, 같은 consumer에서 in-flight 결과의 유한한 public error, 자동 재전송 금지, crash 전파 구간과 descriptor 제거 뒤 생존 provider B의 신규 request 처리를 검증해야 한다. 현재 로그는 graceful 종료인 RM-B2만 증명한다. |
| RM-C1 | implemented | request reply와 send command marker를 provider evidence에서 확인한다. |
| RM-C2 | implemented | 존재하는 RID의 target request와 member snapshot에 없는 RID의 실패 terminal을 확인하며, 지정한 provider만 handler evidence를 남기는지 검증한다. |
| RM-C3 | implemented | direct consumer role endpoint가 두 provider endpoint를 manual peer로 등록해 batch request 분산을 검증한다. |
| RM-C4 | implemented | discovery consumer role endpoint가 timeout을 관측하고 후속 request evidence를 확인한다. |
| RM-C5 | implemented | missing packet request/send 뒤 dispatch-error evidence와 정상 request 회복을 확인한다. |
| RM-C7 | implemented | Redis location store 동적 cluster에서 provider weight를 public runtime option으로 설정하고 discovery consumer가 high-weight provider 선호를 검증한다. |
| RM-C8 | implemented | RouteMesh SS에 Framework-level `MaxMessageSize`를 설정하지 않고 1 byte, 4KiB, 256KiB, 1MiB payload 왕복을 length와 SHA-256 hash로 검증한다. StreamNode의 inbound 상한은 별도 계약이다. |
| RM-C9 | 전환 필요 | 현재 runner는 다량 one-way send 제출과 backlog 해소 뒤 후속 request 회복만 검증한다. non-blocking submit의 즉시 backpressure 결과와 blocking submit의 bounded admission 결과를 public send call에서 직접 대조해야 한다. |

## 검증 결과

- 위 `RM-C8`의 통과 범위는 RouteMesh SS payload round-trip이다. StreamNode의 Core STREAM inbound
  상한은 이 scenario의 대상이 아니며, StreamNode runtime·unit test에서 별도로 검증한다.
- 위 `RM-C9`는 one-way send pressure와 recovery 증거를 보존하되, public submit result 단언을
  추가한 후에만 완료로 바꾼다.
- `all` runner는 Redis location store endpoint와 key prefix를 provider, consumer, workflow role에
  전달한다. 별도 registry process는 구성하지 않으며, scenario별 isolated sub-run은 동적 scenario와
  공통 role이 같은 Redis prefix를 공유하지 않게 한다.
