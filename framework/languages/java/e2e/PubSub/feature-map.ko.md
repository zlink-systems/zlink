# Java PubSub E2E feature map

기준 문서는 `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`다. 모든 시나리오는
Java public framework API와 typed JSON message를 사용한다.

## 마지막 전체 검증

- 명령: `timeout 900s ./run_e2e.sh all`
- 결과: `pub-sub java all result=passed selectors=24`
- 전체 출력: `logs/all-final-20260806.log`
- 실행 시각: 2026-08-06 03:53:38~03:57:43 KST

`all`은 아래 24개 selector를 각각 fresh process와 전용 Redis fixture로 실행한다. A/B/C legacy
batch에서 끝나지 않고 D/E/F selector까지 모두 순회한다.

| 시나리오 | 상태 | 실제 log | 검증 내용 |
|---|---|---|---|
| PS-A1 | 통과 | `logs/20260806-035343-651877` | 세 subscriber가 공통 sequence를 처리한다. |
| PS-A2 | 통과 | `logs/20260806-035355-652741` | 서로 다른 typed packet handler가 자기 event만 처리한다. |
| PS-A3 | 통과 | `logs/20260806-035403-653573` | late subscriber는 구독 전 event를 replay하지 않고 이후 event만 받는다. |
| PS-A4 | 통과 | `logs/20260806-035410-654528` | subscriber 복구 뒤 subscription과 typed delivery가 유지된다. |
| PS-B1 | 통과 | `logs/20260806-035418-655276` | 느린 subscriber와 관계없이 다른 subscriber의 delivery가 진행된다. |
| PS-B2 | 통과 | `logs/20260806-035427-656151` | publisher 재시작 뒤 기존 subscriber가 새 event를 받는다. |
| PS-C1 | 통과 | `logs/20260806-035457-680367` | 미등록 packet은 `HANDLER_MISSING/DROP`으로 기록되고 정상 delivery는 계속된다. |
| PS-D1 | 통과 | `logs/20260806-035506-688111` | endpoint 없는 subscriber가 descriptor로 publisher를 발견하고 typed event를 받는다. |
| PS-D2 | 통과 | `logs/20260806-035514-693884` | 다른 ChannelName의 descriptor와 event를 현재 channel에서 제외한다. |
| PS-D3 | 통과 | `logs/20260806-035520-700544` | publisher 두 개가 Ready가 된 뒤 한 process 종료를 public status에 반영한다. |
| PS-D4 | 통과 | `logs/20260806-035532-707903` | 기존 publisher 종료와 새 identity 등록 뒤 replacement delivery를 확인한다. |
| PS-D5 | 통과 | `logs/20260806-035544-716747` | Store 중단 중 established transport를 유지하고 Store 복구 뒤 다시 수렴한다. |
| PS-D6 | 통과 | `logs/20260806-035548-721483` | port 0 actual listener endpoint를 게시하고 replacement endpoint 변경을 반영한다. |
| PS-D7A | 통과 | `logs/20260806-035602-737368` | capacity 1 observer의 bounded coalescing, snapshot resync와 cancel 격리를 확인한다. |
| PS-D7B | 통과 | `logs/20260806-035616-746495` | manual endpoint mutation이 automatic status와 delivery를 변경하지 않는다. |
| PS-E1 | 통과 | `logs/20260806-035628-747365` | Store 없는 manual publisher와 subscriber가 typed event를 전달한다. |
| PS-E2A | 통과 | `logs/20260806-035632-747775` | Store 없는 automatic subscriber를 public configuration error로 거부한다. |
| PS-E2B | 통과 | `logs/20260806-035635-747991` | automatic discovery와 manual endpoint 혼합을 startup에서 거부한다. |
| PS-E2C | 통과 | `logs/20260806-035638-748247` | publisher identity 방식 누락과 fixed RID·prefix 동시 설정을 listener bind 전에 거부한다. |
| PS-F1 | 통과 | `logs/20260806-035643-748582` | automatic과 manual subscriber가 자기 publisher를 Ready로 표시한 뒤 첫 event를 받는다. |
| PS-F2 | 통과 | `logs/20260806-035650-751860` | proxy로 publisher B 수신만 차단해 A의 Ready와 delivery를 유지하고 B 복구를 확인한다. |
| PS-F3 | 통과 | `logs/20260806-035706-762931` | exact reserved liveness topic은 public publish에서 거부하고 더 긴 prefix topic은 전달한다. |
| PS-F4 | 통과 | `logs/20260806-035711-763432` | publisher 하나의 종료가 다른 publisher의 Ready와 delivery를 변경하지 않는다. |
| PS-F5 | 통과 | `logs/20260806-035719-764090` | 15초를 넘는 비구독 traffic 동안 Ready를 유지하고 구독 topic event를 처리한다. |

## Public/runtime 경계

PS-E2C는 automatic publisher가 fixed RID와 RID prefix 중 정확히 하나를 선택하도록
`ChannelRegistration`에서 검증한다. PS-F3는 `ZLinkFanoutClient.publish`의 public runtime 경로에서 exact
reserved topic을 transport 호출 전에 거부한다.

PS-F1을 위해 manual subscriber는 endpoint마다 전용 SUB socket과 liveness 상태를 사용한다. Publisher
beacon은 Location Store 등록 여부와 관계없이 전송한다. Subscriber evidence endpoint는 public fanout
status와 실제 typed handler 결과만 노출하며 raw frame이나 private API를 사용하지 않는다.

Manual reconnect lifecycle을 최종 점검한 뒤 PS-D7B, PS-E1, PS-F1을 다시 실행했다. 각각
`logs/20260806-040705-1005220`, `logs/20260806-040730-1006593`,
`logs/20260806-040742-1007840`에서 통과했다.
