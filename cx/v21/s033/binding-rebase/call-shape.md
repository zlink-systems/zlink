# Binding bench 호출 형태 확인

측정 대상은 각 언어의 기존 `zlink-<lang>` raw binding 구현이다. 이전 언어별 L-step 대조 자료와 원본 벤치 코드를 확인했다. 이번 작업은 binding 기준만 측정하며 framework 단계를 실행하지 않는다.

| 언어 | source send | target receive | target reply | source completion/reply | 위치 근거 |
|---|---|---|---|---|---|
| Java | raw client가 ROUTER로 새 raw header + protobuf body 전송 | raw server가 blocking `recv(NONE)` 1회 | 새 response header/body와 2-part submit | 기존 admission/completion poll 경로 | [Java call-shape](../lad-java-m1/call-shape.md), `lad-java/framework/bench/grpc/java/{client,zlink-raw-server}` |
| .NET | raw client가 raw header + protobuf body 2-part submit | `ZLinkRawServer` blocking `Recv(None)` 1회 | 새 protobuf body와 raw response header 2-part submit | 공통 raw client completion/reply decode | [Dotnet call-shape](../lad-dotnet-m1/call-shape.md), `lad-dotnet/framework/bench/grpc/dotnet/{Client,ZLinkRawServer}` |
| Node | raw client가 raw envelope/body를 새로 구성해 submit | raw server 재사용 `Received`, `DontWait` batch pump + `setImmediate` 재예약 | 새 protobuf body와 2-part reply | 공통 completion poll/promise 및 reply decode | [Node call-shape](../lad-node-m1/call-shape.md), `lad-node/framework/bench/grpc/node/{client,zlink-raw-server}` |
| C++ | raw driver가 새 envelope/body를 만들어 submit | raw server blocking `recv(none)` 1회 | 새 envelope/body와 2-part reply | completion observer/poller가 reply decode | [C++ call-shape](../l3868-m1/call-shape.md), `l3868/framework/bench/grpc/cpp/{client,zlink}` |

각 binding의 기존 경로를 그대로 사용한다. 구현 차이에 따른 수신 반복 방식은 language binding 고유 구현이다. 고정 handler 외 Framework 호출이나 단계 구현은 측정 경로에 없다. 상세 비용·호출 위치는 위 언어별 대조표를 따른다.
