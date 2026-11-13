# #4350 ladder call-shape 요약

| 언어 / 단계 | 같은 행 | 복원 기능 행 | 복원 기능이 아닌 차이 |
|---|---:|---:|---|
| C++ L0 | 7 | 0 | 없음 |
| .NET L0 | 6 | 0 | source send: L0는 raw legacy request/command header 대신 Framework request header bytes 사용 |
| Java L0 | 6 | 0 | target receive: 첫 수신 뒤 `DONT_WAIT` drain |
| Java L1 | 6 | 1 (L1 permit) | target receive: 첫 수신 뒤 `DONT_WAIT` drain |
| Java L2 | 5 | 3 (L1 permit, L2 source envelope header, L2 target envelope header decode) | target receive: 첫 수신 뒤 `DONT_WAIT` drain |
| Node L0 | 7 | 0 | 없음 |

집계는 표의 call-shape 행 기준이다. Java L1/L2의 L1 permit은 target ingress permit acquire/release 경로 한 행으로 계산했다. Java L2 envelope header는 source encode와 target decode 두 행이다. source completion 행은 그 reply 경로가 존재하는 request 측과 binding completion이 있는 command 측의 공통 소유 경로를 가리킨다.

## 확인 메모

- Java mismatch는 세 ladder step 모두 raw `RecvFlags.NONE` 반복과 달리 첫 blocking receive 뒤 `RecvFlags.DONT_WAIT`로 drain하는 target receive loop다. 복원 feature로 분류되지 않는다.
- C++ L0 실제 경로는 `raw_mesh_node_owner_t::ablation_receive`이며 `recv_flags_t::none`으로 한 번에 한 메시지를 받는다. `--ladder-raw-mesh 1`의 `dontwait` 직접 서버 분기는 L0 측정 경로가 아니다.
- .NET L0 비교 커밋은 receive fix 이후이며 raw와 ladder target 모두 `RecvFlags.None`에 receive timeout을 둔다.
- Node L0 raw와 ladder target은 모두 `DontWait`와 batch pump를 사용한다.
