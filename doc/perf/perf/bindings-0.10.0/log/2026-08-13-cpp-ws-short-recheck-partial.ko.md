# C++ WS short 재측정 partial 결과

release Core `0.10.1`에서 C 후 C++을 순차 실행했다. 공통 조건은
`MULTI_DEALER_DEALER`, ws, clients `100`, duration `2초`, runs `1`, I/O threads
`4/4`, balanced auto-HWM, send/receive timeout `200ms`다.

- C report: `/tmp/zlink-cpp-ws-recheck-c/multi/report/perf_c_multi_linux_20260813_044934_cpp-ws-recheck-c.txt`
- C++ report: `/tmp/zlink-cpp-ws-recheck-cpp/multi/report/perf_cpp_multi_linux_20260813_044956_cpp-ws-recheck-cpp.txt`

완료한 size의 C++/C 비율은 `95.40 / 94.54 / 100.00 / 93.05 / 80.59%`였다.
64KiB 단계 뒤 C++ client가 `errno=111`로 종료되어 128KiB result가 없고 report는
`status: partial`, result line `25/30`이다. 전체 size 평균을 계산하거나 formal 표를
갱신하지 않았다. 종료 실패는 C++ WS close/stop-token 경로에서 별도로 조사한다.
