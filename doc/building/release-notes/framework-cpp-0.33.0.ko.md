[English](./framework-cpp-0.33.0.md) | [한국어](./framework-cpp-0.33.0.ko.md)

# ZLink C++ Framework 0.33.0 릴리스 노트

Framework 0.33.0은 C++ binding 1.26.0과 Core 1.26.0을 사용합니다. (#4846)

## 호환성 변경

- Spot·Actor handler owner type은 등록한 구체 type과 같아야 합니다. 다르면 명시 등록과 scan 등록 모두 startup 설정 오류입니다. 공통 부모 type handler는 구체 type별 handler로 나누고 공통 로직은 일반 메서드로 호출하십시오. (#4548, #4617)
- Spot은 자기 lifecycle callback(Join·Leave 등) 안에서 자기 close를 호출할 수 없습니다. 호출 시점에 InvalidOperation으로 끝납니다. callback에서는 짧은 timer를 등록하고 timer handler에서 close를 기다리지 않고 호출합니다. sample Bingo(다섯 언어)와 Node TicTacToe가 이 방식을 씁니다. (#4432, #4709, #4801)
- channel header를 JSON에서 protobuf(proto3)로 바꿨습니다. wire 형식이 바뀌므로 0.33.0 노드와 0.32.x 이하 노드는 같은 mesh에서 함께 쓸 수 없습니다. 혼합하면 hello에서 거절됩니다(required capability framework-service-v13 → v14). mesh의 모든 노드를 함께 올리십시오. (#4352, #4713)
- 서버 Framework의 라이선스가 zlink License 1.0에서 Elastic License 2.0으로 바뀝니다. client connector, 엔진 adapter, http-client, sample, template의 Apache-2.0과 Core·binding의 MPL-2.0은 그대로입니다. (#3928, #4697)

## 동작 변경

- Core 1.26.0이 `zlink_disconnect`는 connect 등록만, `zlink_unbind`는 bind 등록만 끝내도록 고칩니다. 교체된 node의 이전 descriptor endpoint를 disconnect하면 같은 endpoint에 bind한 새 node의 listener와 inbound 연결까지 닫히던 문제가 사라집니다. (#4846, #4864)
- 등록된 RouteMesh·ClientServer·automatic fanout의 status 조회와 관찰이 runtime 시작 전·시작 중에도 snapshot(`Starting`, IsReady=false)을 반환합니다. 이전에는 언어마다 InvalidOperation 등으로 끝났습니다. 시작 중 status 예외를 잡던 readiness 코드는 지워도 됩니다. (#4800, #4812)
- 닫힌·비활성 target Spot으로 가는 Actor Join은 네 언어 모두 Unavailable로 거절합니다. resolver가 Closing authority를 확인한 Actor Join은 그 사이 activation이 사라져도 NotFound로 끝납니다. (#4510, #4577, #4825, #4831)
- caller 취소·deadline 결과는 local admission을 돌려준 뒤에 공개하고, 수락된 request는 FIFO 자리에서 admission을 다시 얻어 실행합니다. (#4286, #4676)
- 공개 call은 host admission 한 곳에서 시작 전에는 InvalidOperation, 정지 뒤에는 ShuttingDown으로 끝납니다. (#4205, #4265)
- 같은 노드 bind도 Actor owner의 중복 bind 판정을 거치며, 중복 bind callback은 Actor lifecycle lane과 Spot execution gate turn에서 실행됩니다. (#4501, #4516, #4771, #4784)
- runtime weight setter는 값만 기록하고 descriptor 게시는 renew 일정이 맡습니다. Fixed RID 첫 시작의 release wait 안에서 setter를 불러도 host 시작이 실패하지 않습니다. (#4858, #4861)
- Instance Spot send·request 실패가 message flow에 target_rid·reason·오류 상세를 남깁니다. (#4291, #4712)
- 소유 서버 정보가 아직 없는 request는 남은 시간 안에서 discovery 갱신을 기다립니다. Location authority scan은 요청 kind로 provider를 좁히고 page 상한·진행 규칙을 네 언어에서 맞춥니다. (#4160, #4307, #4260, #4720)
- 같은 owner에서 다시 만든 Actor로 간 ID direct request를 current Ready가 처리합니다. (#4520, #4601)
- Actor request deadline이 위치 조회 시간을 포함하고, 조회 뒤 terminal이면 제출하지 않습니다. Actor create와 Instance Spot은 authority 세대를 기다리고, request admission에는 authority가 확정한 owner 세대를 넘깁니다. (#4543, #4592, #4335, #4400, #4428)
- 원격 Entry Spot join이 대상 descriptor의 lifecycle generation으로 기존 canonical Actor join을 사용합니다. 만료 peer 정리는 disconnect의 NOT_FOUND를 이미 종료된 것으로 처리합니다. (#4461, #4791, #4301, #4311)
- owner lease admission deadline을 예약 시각에 기록하고 실행 판정이 그 기록을 읽습니다. 신규 Actor 배치 후보는 live descriptor 목록에서만 고릅니다. (#3877, #4216, #4205, #4217)
- Location Store outage 동안의 소유권 처리를 추가합니다. owner lease TTL은 30초이고 release 대기는 세 margin을 따릅니다. outage 보존 옵션·episode 관찰, 보존 gate와 T 경과 뒤 폐기, 복구 뒤 보존 객체 검증·재개, outage 중 lease를 정상 만료 뒤에만 다시 얻는 규칙, 두 번째 outage의 새 episode 처리를 포함합니다. (#3877, #3909, #3946, #3975, #3993, #4020, #4029, #4048, #4055, #4058, #4068, #4269)
- etcd Location Store provider를 opt-in extension으로 추가합니다. (#4081, #4137)
- lease가 outage 밖에서 stale로 확인된 renew는 실행을 폐기합니다. (#4625)

## 결함 수정

- message flow 추적을 켠 상태에서 두 Spot의 상대 Join이 동시에 commit하면 멈출 수 있던 문제를 고칩니다. actor transfer marker는 이제 lane이나 dispatcher를 기다리지 않습니다. (#4857, #4865)
- standalone host가 자기가 만든 Core context를 close에서 끝냅니다. (#4862, #4869)
- Location Store 없이 manual endpoint만 쓰는 ClientServer client도 hello로 승인받습니다. (#4845, #4847)
- stream connector wait가 queue를 확인하는 중 도착한 packet을 놓치지 않고 등록합니다. Stream write가 transport 완료 task를 다시 await하지 않아 종료 뒤 frame이 Stream host를 붙잡지 않습니다. (#4840, #4154, #4746)
- manual fanout subscriber가 handler가 suspend하면 worker를 반환합니다. suspended handler가 application worker를 쥐지 않고 record 수명과 Actor handoff 완료를 바로잡습니다. (#4772, #4786, #4448, #4618)
- run 종료 뒤 종료 결과를 재사용합니다. handler, filter, channel owner를 dispatch마다 만듭니다. Node와 Channel의 same-owner claim을 독립 실행합니다. (#4420, #4439, #4145, #4161, #1639, #4313)
- 생성한 ZLJR codec이 길이를 먼저 읽고 끝 위치를 계산해 Clang에서 source를 거부하지 않습니다. (#4275, #4365)
- Redis Location Store snapshot scan이 Redis 시계로 만료를 판정하고, 뒤 page도 첫 page의 시간을 돌려줍니다. (#4067, #4655)

## 성능 개선

- 기본 typed JSON이 yyjson으로 읽고 씁니다. float와 bytes 인코딩의 임시 문자열 allocation을 줄이고 byte payload 처리를 정리했습니다. (#4354, #4826, #4427)
- envelope header writer·reader·decode의 복사, 문자열 소유, 검증 순회를 줄입니다(L1→L2 비용을 7% 안으로 유지). (#4352, #4443, #4466, #4513)
- request 경로의 submit 할당·복사를 줄이고, receiver를 한 block이 소유하며, Node request header를 service frame에 한 번만 인코딩합니다. 받은 record를 하나의 canonical record로 받고 source 쪽 중복 send 작업을 없앱니다. (#3867, #3884, #3888, #3912, #3919, #3957, #4014, #4124, #4155, #4256)
- 경합한 source 제출을 lane의 기존 drain이 실행합니다. completion callback은 고정 worker 두 개로 실행하고 target receive role은 rotation 경계에서 정합니다. (#4355, #4805, #1639, #4179, #4302)
- Ready route cache miss가 owner lease를 한 번만 읽고 그 절대 기한으로 cache 수명을 정합니다. claim한 mailbox record의 이동 할당과 STREAM dispatch의 packet별 serial_log 기록을 없앱니다. (#4260, #4277, #4375, #4727, #4758, #4780)

## 의존성

- C++ Framework가 내부에서 yyjson 0.12.0을 사용합니다. vcpkg와 Conan 버전을 맞췄습니다. 공개 header에는 드러나지 않으며, 정적 빌드만 link 때 yyjson이 필요하고 설치된 CMake config가 `find_dependency(yyjson CONFIG)`로 찾습니다. (#4354, #4826)
- 배포와 CI가 같은 grpc 1.84.0, abseil 20260107.1, protobuf 6.33 계열을 사용하도록 고정합니다. (#4077, #4559)

## 설치

[GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.33.0)에서 플랫폼별 Framework archive를 선택합니다. Server Framework는 C++ binding 1.26.0과 Core 1.26.0을 사용합니다. Stream Connector만 사용하는 client에는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.33.0)입니다.
