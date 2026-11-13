Binding driver는 pacer가 반환한 due slot 수만큼 제출 coroutine에서 요청을 시작하고, outstanding window와 admission 한도에서 멈춥니다. 위치는 framework/bench/grpc/cpp/client/bench_cpp_client.cpp:641-704입니다.

Framework driver는 각 pacer wake 뒤 반환된 due slot 수만큼 can_submit을 다시 확인해 제출합니다. 위치는 framework/bench/grpc/cpp/client/bench_cpp_client.cpp:1101-1108입니다.

rate 0이면 binding은 기존 run_unbounded 또는 run_slots 경로를 그대로 사용하고 Framework는 기존 한 건 제출 경로를 사용합니다. 위치는 framework/bench/grpc/cpp/client/bench_cpp_client.cpp:528-537 및 1107-1108입니다.

runner 변수는 TARGET_RATE, SOURCE_CPUS, TARGET_CPUS입니다. TARGET_RATE는 client에 --target-rate로 전달하고 SOURCE_CPUS와 TARGET_CPUS는 설정 시 각각 source와 target process에 taskset -c를 적용합니다. 위치는 framework/bench/grpc/cpp/run_local.sh:3,65-91 및 framework/bench/grpc/README.ko.md:654-663입니다.

빌드는 지정 슬롯에서 20분 동안 시작되지 않아 SIGINT로 중단했습니다. 상태는 ready to compile입니다. 벤치는 실행하지 않았습니다.

bash scripts/format/format.sh --check cpp, bash -n framework/bench/grpc/cpp/run_local.sh, git diff --check는 통과했습니다.

변경 diff는 pacer/change.diff에 저장했습니다.
