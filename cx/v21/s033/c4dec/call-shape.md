# Decode 분리 probe 호출 대조

Ladder source send: raw bench_cpp_client.cpp:455-472,651-654 → ladder_driver.inc:121-147 (62e766f88d). Target receive: raw bench_zlink_cpp_server.cpp:41,54-56 → raw_mesh_node_owner.cpp:530-546, ladder_wire.hpp:253-269. Reply: raw bench_zlink_cpp_server.cpp:58-67 → ladder_wire.hpp:278-315. Completion: raw bench_cpp_client.cpp:623-630 → ladder_wire.hpp:324-348. L4 차이는 기본 typed JSON 복원이다.

이 작업은 네트워크·throughput 측정을 하지 않는다. 이미 만든 body를 warmed default registry로 decode하는 비용만 센다. L4 요청·응답 DTO는 같은 typed_payload_t (ladder_payload.hpp:15-22,37-38, raw/ladder_payload.hpp)다. wait·metrics atomic·framing은 측정 범위 밖이다. CPUs 6-7,14-15, perf run 4초, measure.lock 사용 없음.
