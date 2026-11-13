# Java ladder L4 구현 보고

| 단계 | 복원 내용 및 production 경로 | Bench 코드 | commit | 검증 | 호출 형태·재개 후 측정 |
|---|---|---|---|---|---|
| L4 (#4354) | 기본 typed JSON payload codec. 송신 `ZLinkPayloadEncoding.java:49-71`; 수신 `ZLinkChannelHandlerInvoker.java:421-429`; reply encode `:393-396`; source reply decode `ZLinkChannelReplyDecoder.java:47-49`. | `RawStack.java:402-413,415-428`; target `ZLinkFrameworkBenchServer.java:239-316`; switch/client selection `FrameworkStack.java:132-143`. 기존 L0-L3는 protobuf 경로 유지. | `18a25db605` (`exp/4350-java-ladder`, 로컬만) | `framework/bench/grpc/java/build.sh` 성공. `:client:ladderTypedPayloadTest` 성공. client/server/shared class와 JAR이 해당 source보다 최신. L4 서버 dry start 성공, `/ready` 응답 확인 후 부하 없이 종료. 측정·measure.lock 미사용. | [call-shape-L4.md](call-shape-L4.md): binding 대 L4의 source send·target receive·target reply·source completion을 비교. 의도한 차이는 이전 단계 L1 permit, L2 envelope, L3 service wire와 이번 단계 typed codec뿐. 측정 재개 시 **L3 대 L4**, send-saturation/request-serial/request-backpressure 각 1K·4K의 6 cells. |

## L5 (#4355)

미구현. Java production은 `ZLinkChannelReceiveLoops`에서 `ZLinkMeshApplicationDispatcher`로 넘기고, owner queue 진입은 package-private `ZLinkHandlerDispatch.enqueue`를 거친다. 그 경로를 쓰려면 L6 activation/L7 operation까지 함께 켜거나 내부 경로를 bench에 복제해야 해 L5만 분리되지 않는다. 공개된 `ZLinkSerialExecutionQueue`만 직접 호출하는 대체도 production dispatch 경로와 같다고 볼 수 없어 추가하지 않았다.
