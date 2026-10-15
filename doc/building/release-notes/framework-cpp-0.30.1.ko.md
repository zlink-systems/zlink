[English](./framework-cpp-0.30.1.md) | [한국어](./framework-cpp-0.30.1.ko.md)

# ZLink C++ Framework 0.30.1 릴리스 노트

Framework 0.30.1은 C++ binding 1.19.0과 Core 1.19.0을 사용합니다.

## 변경

- 서버 Framework의 라이선스가 zlink License 1.0으로 바뀌며, 1.0.0 전 버전은 키나 유료 라이선스 없이 머신 수와 관계없이 무료입니다. Client connector·공용 client 코드·sample·tutorial·quickstart와 HTTP client는 Apache-2.0이며, 0.30.0 이하 릴리스의 라이선스는 바뀌지 않습니다. (#1681)
- 공개 API 추가: request 결과 `request_task_t<TReply>`에 `admitted()` 단계를 제공합니다. Reply를 기다리지 않고 해당 request의 source-local 수락을 기다릴 수 있으며, 수락 전에 실패하면 admission과 reply가 같은 원인으로 실패합니다. (#1682)
- 공개 API 추가: `framework_runtime_status_t.safe_to_shutdown`으로 relocation source의 종료 가능 여부를 조회하고 관찰할 수 있습니다. 이동 후 source 작업이 정리되면 상태 변경도 게시합니다. (#1853)
- 메시지마다 topology·owner lease 조회를 위해 lane 완료를 기다리던 경로와 중복 completion 게시를 줄였습니다. 수신 header는 ingress에서 한 번 해석하고 payload는 중간 복사 없이 전달합니다. (#1650, #1662, #1665)

## 결함 수정

- 잘못된 envelope를 받은 request가 언어에 따라 오류 응답 없이 끝나거나 다르게 처리되던 결함을 수정했습니다. Header가 문법에 맞는 JSON object이고 유효한 correlation ID와 reply token이 있으면 오류 응답을 보내며, reply 경로를 확인할 수 없으면 응답 없이 관측 정보에 기록합니다. (#1671)
- Spot Configure의 잘못된 handler·subscription 등록을 activation의 Ready 전에 `InvalidOperation`으로 거부하도록 네 언어의 오류 분류를 맞췄습니다. (#1715)
- Relocation이 전달하는 새 owner generation을 Store CAS 결과와 일치시키고 sealed Session binding을 함께 전달합니다. 이동 후 Session route를 확정할 때 서로 다른 generation이나 누락된 binding 상태를 사용하던 경로를 수정했습니다. (#1807)
- 같은 Instance Spot의 cold activation에 합류한 요청이 첫 요청의 recovery 기록 삭제 뒤 `DataLost`로 실패하던 결함을 수정했습니다. 합류 요청은 수락된 activation 입력을 사용하며 metadata와 payload 일치 검사는 유지합니다. (#1653)
- Apple libc++에서 지원하지 않는 atomic shared pointer 사용으로 macOS arm64 prebuilt 빌드가 실패하던 결함을 수정했습니다. (#1667)
- 빈 reply를 가진 deferred Actor Join의 recovery가 reply metadata 누락으로 거부되던 결함을 수정했습니다. 빈 reply는 metadata 없이 보존하고 복구합니다. (#1746)
- ClientServer request가 Core 제출 전에 준비 부족이나 deadline 만료로 끝나도 `admitted()`가 성공하던 결함을 수정했습니다. Admission과 reply가 같은 실패로 끝납니다. (#1749)
- 진행 중인 request의 route client를 해제할 때 worker가 자기 종료를 기다려 builder 해제나 shutdown이 멈추던 결함을 수정했습니다. 실행 자원은 runtime 종료 시 정리하며, idle 상태로 종료한 worker도 회수합니다. (#1768, #1789)
- `Yield()`가 handler의 실제 suspend 전에 execution gate를 반납하거나 facade 호출에서 반납하지 않던 결함을 수정했습니다. 재개 뒤에도 같은 queue의 새 turn을 사용하므로 연속 Yield와 `Defer()`가 동작하며, owner 종료 시 대기도 해당 owner가 완료합니다. (#1789)
- Route 교체 중 cross-node bind·create·Join과 relocation이 멈추던 결함을 수정하여, 다른 선택 route를 관찰하면 ready 상태에서 durable request와 batch·cutover를 다시 제출합니다. ADMIT까지 같은 HELLO snapshot을 재전송하고 이후 descriptor 변경은 UPDATE로 보내며, 기존 route나 같은 상대의 manual intent가 있으면 자동 중복 연결을 만들지 않습니다. (#1814, #1851, #1871)
- Join completion을 기다리는 동안 돌아온 Actor에 local send를 보내면 `NotFound`로 거부되던 결함을 수정했습니다. 현재 ActorId로 대상을 찾고 Actor queue와 execution gate가 실행 시점을 정합니다. (#1823)
- 초기 owner lease claim이 오래 걸리면 첫 heartbeat가 늦어지던 결함을 수정했습니다. 첫 갱신도 claim operation 시작 시각을 기준으로 예약합니다. (#1830)
- Deferred Actor Join completion callback의 일반 예외가 프로세스를 `std::terminate`로 종료하던 결함을 수정했습니다. 원래 예외를 전달하고, target 실행 실패는 `Failed.Kind`로 구분하며 정상 `Rejected`의 optional reply도 local·remote 모두 보존합니다. (#1844)
- Relocation target이 commit 직후 종료해 authority가 `Missing`이 되면 source 정리와 host close가 계속 기다리던 결함을 수정했습니다. Expired-owner terminal로 종료하며, Session route 확정에서는 Session owner가 소유한 identity·binding 검증만 수행합니다. (#1847)
- Actor member가 있는 host의 shutdown이 `ForceStopped/TeardownFailed`로 끝나거나 maintenance Entry Actor 복원·application 신호 기반 SpotWide capture가 실패·정지하던 결함을 수정했습니다. 종료는 local Actor를 정리하고 maintenance는 target startup Entry를 사용하며, capture는 자신이 보유한 turn에서 진행합니다. (#1853)
- 이동 중인 User Spot의 create 거부가 Busy terminal로 전송되던 결함을 수정했습니다. `SpotMoving`은 Conflict terminal로 전달합니다. (#1715)

## 설치

[`framework-cpp/v0.30.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.1)에서 플랫폼별 Framework archive를 선택하거나 `bootstrap.cmake`로 내려받습니다. 서버 Framework에는 C++ binding 1.19.0과 Core 1.19.0이 필요합니다. Stream Connector만 사용하는 client에는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.30.1)입니다.
[한국어](./framework-cpp-0.30.1.ko.md) | [English](./framework-cpp-0.30.1.md)
