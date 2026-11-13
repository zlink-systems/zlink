### 1.5 Framework 바깥의 미확정 항 — Core I/O thread(Δ_native)

R §1은 ccall2에서 target I/O1이 0.813 core였다고 적는다(`R:48`). 같은 ccall2의 caller는 19.078 µs/request에 0.824 core이므로 처리율은 `0.824 / 19.078e-6 ≈ 43,200 msg/s`이고, I/O1의 메시지당 CPU는 `0.813 / 43,200 ≈ 18.8 µs`다. binding 벤치는 249,415 msg/s를 I/O thread 1 core 이하로 처리하므로 I/O thread는 메시지당 `≤ 4.01 µs`다. **가설: Framework 경로에서 Core I/O thread의 메시지당 CPU가 binding 경로의 4배 이상이다.** 원인 후보는 frame 수·byte 수 증가(frame 0 + JSON header + body, reply도 같다), worker thread에서 reply마다 동기 submit(`C/backend/raw_route_port.cpp:509-535`)이 I/O thread를 깨우는 횟수, Completion connection 경로다. 이 값이 사실이면 Framework 쪽 µs를 모두 줄여도 target I/O thread가 `10^6 / 18.8 ≈ 53k msg/s`에서 처리량을 막는다 — 현재 F 51,549와 거의 같다. **0.8 설계의 첫 측정 항목이다**(§6).

