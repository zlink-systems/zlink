| 부분 | ccall 이전 µs/request | PR 1 µs/request | 변화 | 예산의 기대 변화 |
|---|---:|---:|---:|---:|
| 인코딩 | 2.15 | 2.12 | −0.03 | 0.00 |
| 등록 | 4.42 | 5.76 | +1.34 | 결과 관찰과 합쳐 −5.38 목표, 개별 배분 없음 |
| 결과 관찰 | 4.25 | 2.76 | −1.49 | 등록과 합쳐 −5.38 목표, 개별 배분 없음 |
| binding 제출 | 5.54 | 5.00 | −0.54 | 0.00 |
| 기타 | 1.11 | 0.97 | −0.14 | 0.00 |
| 합계 | 17.48 | 16.61 | −0.87 | −5.38 |

PR 1은 예산보다 caller에서 4.51 µs/request 덜 줄었다. ccall의 stage 매핑을 그대로 적용했다. ccall 이전 단계 값은 17.48 µs/request에 맞춰 정규화한 보고값이다. PR 1도 profile 단계 비율을 같은 방식으로 정규화했으며, PR 1의 무프로파일 실행에서 측정한 caller 비용 16.612 µs/request를 기준으로 했다. profile caller는 19.078 µs/request였고, profile 자체의 부담을 제거하기 위해 stage 값만 무프로파일 비용으로 정규화했다. PR 1 등록과 결과 관찰의 합은 8.67에서 8.52 µs/request로 0.15 µs만 감소했다. 등록이 1.34 µs 증가해 결과 관찰의 1.49 µs 감소를 거의 상쇄했다. 이 두 stage에서 예산의 5.38 µs 감축 중 실제로 확인된 양은 0.15 µs다.

무프로파일 실행은 52,066 request/s였고 submit thread는 0.865 core, 16.612 µs/request였다. PR 1의 ratio048p1 3회 중앙값 51,767 request/s와 caller 16.34 µs/request에 가깝다. 실행에서 completed와 received는 각각 1,041,902로 같았고 errors, abandoned, rejected는 모두 0이었다. 프로파일 실행 처리율은 43,197 request/s였으며, 8.04초 capture 동안 347,325개 메시지를 관찰했다. profile caller는 0.824 core, 19.078 µs/request였다. 조건 B의 source affinity는 CPUs 0–7, target affinity는 CPUs 8–15였다. capture 시 MemAvailable은 12 GiB 이상이었다. raw profile 파일은 source 1008 KiB, target 880 KiB로 1 GB 미만이다.

| 순위 | ccall 이전 file:function | self / inclusive µs | PR 1 file:function | self / inclusive µs |
|---:|---|---:|---|---:|
| 1 | libc.so:_int_malloc | 1.0765 / 1.2853 | framework/bench/grpc/cpp/client/bench_cpp_client.cpp:framework_driver_t::run_tasks | 0.9848 / 12.7353 |
| 2 | bench_cpp_client:[unknown] | 0.8033 / 15.9541 | bench_cpp_client:[unknown] | 0.8012 / 15.6730 |
| 3 | framework/bench/grpc/cpp/client/bench_cpp_client.cpp:framework_driver_t::run_tasks | 0.6266 / 12.6926 | libc.so:_int_malloc | 0.6343 / 0.8679 |
| 4 | libc.so:__memmove_avx_unaligned_erms | 0.5302 / 0.5302 | libc.so:pthread_mutex_lock | 0.5508 / 0.5508 |
| 5 | libstdc++.so:std::_Sp_counted_base::_M_release | 0.4981 / 0.5623 | libc.so:__memmove_avx_unaligned_erms | 0.5007 / 0.5007 |
| 6 | libc.so:malloc | 0.4338 / 1.7191 | kernel:__schedule | 0.4507 / 0.5174 |
| 7 | kernel:__schedule | 0.4177 / 0.4177 | framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:route_client_t::submit_request_reply_message_erased | 0.3672 / 6.9268 |
| 8 | libc.so:pthread_mutex_lock | 0.4177 / 0.4177 | libc.so:malloc | 0.3505 / 1.2018 |
| 9 | libstdc++.so:std::__shared_count::__shared_count(copy) | 0.3213 / 0.3213 | libzlink.so:zlink_request | 0.3004 / 0.5007 |
| 10 | kernel:_raw_write_unlock_irq | 0.3053 / 0.3053 | kernel:do_syscall_64 | 0.3004 / 1.6524 |
| 11 | framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:header_writer_t::append | 0.3053 / 0.3053 | framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:header_writer_t::append | 0.2671 / 0.2671 |
| 12 | libc.so:pthread_mutex_unlock | 0.2410 / 0.2410 | framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:client_call_codec_t::create_envelope | 0.2671 / 0.3672 |
| 13 | libc.so:_int_free_merge_chunk | 0.2410 / 0.2571 | framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::request_impl | 0.2504 / 0.5341 |
| 14 | framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:client_call_codec_t::create_envelope | 0.2249 / 0.2892 | libc.so:pthread_mutex_unlock | 0.2337 / 0.2337 |
| 15 | kernel:_raw_spin_unlock_irqrestore | 0.2249 / 0.2249 | libstdc++.so:std::_Sp_counted_base::_M_release | 0.2170 / 0.2170 |
| 16 | libc.so:_int_free | 0.2249 / 0.5945 | libc.so:_int_free_merge_chunk | 0.2003 / 0.2003 |
| 17 | libzlink.so:zlink_request | 0.2089 / 0.4017 | libc.so:cfree | 0.2003 / 0.6343 |
| 18 | libc.so:cfree | 0.2089 / 0.8355 | libc.so:malloc_consolidate | 0.2003 / 0.2003 |
| 19 | framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:header_writer_t::string | 0.1928 / 0.2249 | libzlink.so:zlink_msg_size | 0.2003 / 0.2003 |
| 20 | framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:raw_route_port_t::request_impl | 0.1767 / 0.6105 | libzlink.so:zlink::router_socket_t::request | 0.1836 / 0.1836 |

템플릿 인자와 clone 표기는 읽기 쉽게 줄였다. 함수별 값은 각 profile의 caller thread CPU로 정규화한 표본 추정치다. inclusive 값은 겹치므로 더하지 않는다. PR 1의 새 task owner 심볼은 다음 표에 별도로 모았다.

| PR 1 caller 심볼 | self / inclusive µs/request | 위치 |
|---|---:|---|
| task_ptr_t<prepared receiver, 4096>::allocate_task_owner | 0.1502 / 0.2504 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:allocate_task_owner |
| task_control_t::release | 0.1168 / 0.6676 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:task_control_t::release |
| request_callback_t::from_shared | 0.0835 / 7.3274 | framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:request_callback_t::from_shared |
| make_task_observer<task_shared_state_t::observer_t> | 0.0835 / 0.3672 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:make_task_observer |
| allocate_task_owner<prepared receiver, 4096>::destroy closure | 0.0668 / 0.4340 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:allocate_task_owner |
| task_ptr_t<route callback> | 0.0668 / 0.4674 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:task_ptr_t |
| make_task_observer<callback lifetime> | 0.0501 / 0.0835 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:make_task_observer |
| std::pmr::synchronized_pool_resource::do_deallocate | 0.0334 / 0.1335 | libstdc++.so:std::pmr::synchronized_pool_resource::do_deallocate |
| task_memory_resource_t::do_allocate | 0.0167 / 0.1335 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:task_memory_resource_t::do_allocate |
| std::pmr::synchronized_pool_resource::do_allocate | 0.0167 / 0.0334 | libstdc++.so:std::pmr::synchronized_pool_resource::do_allocate |
| allocate_task_owner<prepared receiver, 4096>::deallocate closure | 0.0167 / 0.2337 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:allocate_task_owner |
| allocate_task_owner<route callback, 512> | 0.0167 / 0.0835 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:allocate_task_owner |
| task_arena_allocator_t / task_weak_t | 표본 없음 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp |
| pthread_mutex_lock 전체 | 0.5508 / 0.5508 | libc.so:pthread_mutex_lock |
| pthread_mutex_unlock 전체 | 0.2337 / 0.2337 | libc.so:pthread_mutex_unlock |
| task_memory_resource_t의 arena mutex 경로 | lock 0.0167, unlock 0.0334 | framework/languages/cpp/framework/include/zlink/framework/detail/task_owner.hpp:task_memory_resource_t::do_allocate |
| std::condition_variable::condition_variable | 0.0501 / 0.0501 | libstdc++.so:std::condition_variable::condition_variable |
| std::pmr::monotonic_buffer_resource::~monotonic_buffer_resource | 0.0501 / 0.0501 | libstdc++.so:std::pmr::monotonic_buffer_resource::~monotonic_buffer_resource |

PR 1 task owner와 그 callback/observer 표본의 self 합은 0.6843 µs/request다. 기존 shared_ptr control block 프레임도 caller에서 완전히 사라지지는 않았다. `_Sp_counted_base::_M_release`는 0.4981에서 0.2170으로, `__shared_count` 복사 생성은 0.3213에서 0.0835로 감소했다. 제거·감소한 두 프레임 합은 약 0.52 µs다. `task_control_t::release`, `task_ptr_t`, observer 소유 구성으로 새로 표본화된 비용이 그 절감의 일부를 다시 차지한다.

할당 프레임도 줄었지만 없어지지 않았다. `_int_malloc`은 1.0765에서 0.6343으로 0.4422 µs 줄었고, `malloc`은 0.4338에서 0.3505로 0.0833 µs 줄었다. 반면 PR 1은 synchronized pool 호출, arena 할당 mutex, intrusive strong/weak 참조 처리를 추가했다. `pthread_mutex_lock` caller self는 0.4177에서 0.5508로 0.1331 µs 늘었지만 이 기호에는 기존 socket·task lock도 포함되어 있으므로 증가분 전부를 pool 또는 arena lock으로 귀속할 수 없다. 직접 보이는 arena lock 구간은 `task_memory_resource_t::do_allocate`의 0.0167 µs와 대응 pthread unlock 표본 0.0334 µs다. pool의 `do_allocate`와 `do_deallocate`는 직접 self 합 0.0501 µs다. 이 표본에서 pool 내부 mutex만 따로 분리한 값은 측정하지 못했다.

Prepared receiver owner는 4096바이트 inline buffer를 쓰고 일반 callback owner는 512바이트 buffer를 쓴다. 큰 블록의 메모리 이동·캐시 비용만 따로 떼어 측정하지는 못했다. 4096바이트 receiver의 owner 생성과 해제 closure에서 직접 관측된 self는 각각 0.1502, 0.0668, 0.0167 µs다. 이 값은 allocation, 초기화, intrusive owner 생성·해제가 합쳐진 값이지 block 크기만의 비용은 아니다. condition variable은 PR 1에서 새 원시 비용으로 caller에 더해졌다고 볼 수 없다. 이전 profile에도 생성자 0.0643 µs와 notify_all 0.0321 µs가 있었다. 새 profile caller의 생성자 표본은 0.0501 µs였고 caller에서 notify/wait는 표본화되지 않았다. 동기화 객체 소유 위치는 `task_owner.hpp:task_synchronization_t`로 이동했다.

일부 비용은 completion 경로에 남거나 그쪽에서 더 많이 실행됐다. completion owner가 drain하는 `bench_cpp_client` thread는 16.311에서 17.125 µs/request로 0.815 µs 증가했다. 새 `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:observe_request_completion`은 그 thread에서 self 0.3749 µs, inclusive 5.8788 µs로 표본화됐다. 이전의 `task_shared_state_t::complete` caller stack 표본은 asio executor 경로에서 self 0.0535 µs, inclusive 0.4281 µs였다. PR 1에서는 physical observer가 task owner를 보유하므로, owner 정리와 completion 관찰 비용 일부가 submit caller 밖 completion 경로에서 발생하는 흐름과 맞는다. 이 thread 전체 증가량 0.815 µs를 새 observer 하나의 비용으로 단정하지는 않는다.

I/O thread는 전체적으로 큰 이동을 보이지 않았다. `ZLINKbg/IO/0`는 17.209에서 16.419 µs/request로 0.790 µs 감소했고 `ZLINKbg/IO/2`는 14.217에서 14.438 µs로 0.221 µs 증가했다. caller의 binding submit stage 감소 0.54 µs도 확인됐지만 PR 1은 binding/Core submit을 바꾸지 않았으므로 원인 이동으로 판정하지 않는다. 단일 profile 실행 간 변동일 수 있다.

따라서 PR 1이 caller에서 실제로 없앤 shared_ptr와 heap 비용은 약 0.5 µs의 shared-count 표본 감소와 약 0.5 µs의 allocator 표본 감소다. 동시에 새 intrusive owner와 result lock/arena allocation 경로가 caller에 약 0.68 µs 이상의 직접 표본 비용을 남겼고, stage 분류상 등록 비용은 1.34 µs 증가했다. 제거한 admission conversion과 observer/result 작업은 결과 관찰 stage를 1.49 µs 줄였지만, 전체 기대치 5.38 µs의 대부분은 caller CPU에서 사라지지 않았다. 일부 owner 해제와 physical observer 정리는 completion 쪽으로 이어진다.

코드 변경 없음.
