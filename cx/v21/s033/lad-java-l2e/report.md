# Java ladder L2 응답 envelope 보정

| 항목 | 결과 |
|---|---|
| Production 응답 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelDispatchReporter.java:378-397` (`reply(requestHeader)` + prepared-name envelope encode) |
| Bench 응답 | `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:249-269` |
| Production completion parse | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelCallRuntime.java:260-272` (`tryDecodeHeader` 1회) |
| Bench completion parse | `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java:404-433` |
| 새 L2 commit | `e8721a0776` |

재적재 old → new: `c20022b27e → 11f72e06dd`, `6b77f3b04d → 8ec4b2e64f`, `23be466e71 → fea12ea97a`, `18a25db605 → 9d9901eb2c`, `4bf0f1c4bf → f3e3933567`.

빌드 `bash framework/bench/grpc/java/build.sh` 성공. L2에서 `:client:check` 성공. 재적재 tip의 `:client:check :zlink-framework-server:test`는 다른 job의 measure.lock 구간에서 일시 정지되어 완료 대기 중이다. 측정 및 benchmark 실행 없음.
