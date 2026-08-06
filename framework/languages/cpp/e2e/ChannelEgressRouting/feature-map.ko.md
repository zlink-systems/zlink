# C++ ChannelEgressRouting feature map

기준 문서는 `framework/doc/framework/common/e2e/config-12-channel-egress-routing.ko.md`다.
C++ 구현은 `0.10.0` public framework surface와 실제 process E2E runner를 함께 사용한다.
Host lifecycle에 channel 단위 `drain()` public method는 없으므로 CH-E2E-04B는
`app_t::request_stop()`이 시작하는 host `shutdown()`으로 검증한다. 이 shutdown은
ClientServer descriptor를 신규 선택에서 제외하고, 이미 수락한 handler를 완료한 뒤
listener와 descriptor를 정리한다.

| 시나리오 | 상태 | C++ 검증 근거 |
|---|---|---|
| CH-E2E-01 | 구현 | RouteMesh ChannelName의 양방향 request를 두 process에서 확인한다. |
| CH-E2E-02 | 구현 | Play handler가 Audit RouteMesh와 Workflow ClientServer로 nested request를 보내고 두 reply를 보존한다. |
| CH-E2E-03 | 구현 | Instance Spot handler와 timer가 Workflow request/reply를 처리한다. |
| CH-E2E-04A | 구현 | weight `100:300`에서 800개 request의 weight-300 선택 비율을 확인한다. |
| CH-E2E-04B | 구현 | A의 수락된 hold request를 public evidence로 확인하고 host shutdown을 시작한 뒤, A의 완료와 B만 선택되는 신규 50개를 확인한다. |
| CH-E2E-04C | 구현 | 같은 endpoint의 server process를 재시작하고 새 lifecycle marker를 포함한 첫 request를 확인한다. |
| CH-E2E-05 | 구현 | Server-only process의 ClientServer request 거부와 정상 Client process의 성공을 확인한다. |
| CH-E2E-06 | 구현 | duplicate Client role 등록 process가 startup configuration error로 종료되는지 확인한다. |
| CH-E2E-07A | 구현 | 등록하지 않은 ChannelName request가 bounded failure로 끝나는지 확인한다. |
| CH-E2E-07B | 구현 | RouteMesh API channel의 양방향 peer topology와 request를 확인한다. |
| CH-E2E-07C | 구현 | 연결을 끊은 unavailable target request가 성공으로 위장되지 않는지 확인한다. |
| CH-E2E-08 | 구현 | nested RouteMesh와 ClientServer request의 outer reply 및 downstream evidence를 확인한다. |
| CH-E2E-09 | 구현 | RouteMesh와 ClientServer public listener status가 실제 bound port를 보고하고 port `0`을 노출하지 않는지 확인한다. |
| CH-E2E-10 | 구현 | ClientServer one-way send와 server evidence를 확인한다. |
| CH-E2E-11 | 구현 | 같은 process가 보유한 API channel의 request와 send를 확인한다. |
| CH-E2E-12 | 구현 | 같은 process의 local Server와 remote Server가 모두 ClientServer target으로 선택되는지 확인한다. |

runner는 Redis location store를 scenario별 key prefix로 격리하고, role process의 health와 public
evidence를 확인한 뒤 client runner를 실행한다. 각 scenario는
`framework/languages/cpp/e2e/ChannelEgressRouting/run_e2e.sh`에서 독립적으로 실행할 수 있다.
