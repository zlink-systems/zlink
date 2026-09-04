START: detached 053a568ddd 확인, bindings/cpp/** 외 기존 변경 없음(core/build·core/build-dev symlink만 untracked), 가이드 §2·§4 확인
BASELINE_BUILD: Core 0.17.0 외부 symlink 재빌드 없이 C++/C multi 10-client 1024B 1초 빌드·실행 완료; C++ DD 611089 msg/s, RR 29336 ops/s; C DD 929605, RR 270138
PROFILE: callgrind 10-client 1024B 1초 완료; DD C++ 13.47k Ir/msg·3.42 new/msg vs C 9.06k·0.26, RR C++ 50.71k Ir/op·12.57 new/op vs C 23.98k·1.13; DD register/map/entry/future가 즉시 성공 11540건 모두, RR entry+future+map가 1336건 모두 확인
IMPLEMENT: future+completion entry 단일 allocation bundle, completion map node socket-lifetime PMR 재사용, async terminal acquire fast path, SEND는 첫 admission 뒤 backpressure일 때만 owner map 등록하고 early WRITABLE replay 보존
VERIFY_RELATED: request_reply/request_writable_retry/application_ready_queue/optimization_guard 4종 각 5회 PASS; after callgrind DD 12.46k Ir/msg·1.39 new/msg, RR 49.16k Ir/op·10.44 new/op
AFTER_BENCH: 요청 명령 그대로 15/15 완료; DD 64B 740751·1024B 722420 msg/s, DR-RR 64B 86065·1024B 62217 ops/s, RR-RR 64B 88867·1024B 64667 ops/s
GATE: 전체 run_tests.sh PASS(contract 16/16, sample 7/7), 관련 4종 각 5회 PASS, send_close_stress ownership_failures=0/bad_records=0/unexpected=0, diff-check·public-header 확인 PASS
SUMMARY: cpp-perf-pass1-summary.md 작성 완료; tracked 변경 6개 모두 bindings/cpp/**, BLOCKERS 없음(잔여 REQREP gap 및 DR-RR 4096B -2.2% 기록)
EXIT:0
