| 부분 | Framework µs/request | binding µs/request | 6.59 µs를 향한 감축 필요량(Framework µs/request) |
|---|---:|---:|---:|
| 인코딩 | 2.15 | 0.14 | 1.34 |
| 등록 | 4.42 | 0.01 | 2.76 |
| 결과 관찰 | 4.25 | 0.05 | 2.65 |
| binding 제출 | 5.54 | 2.52 | 3.45 |
| 기타 | 1.11 | 0.97 | 0.69 |
| 합계 | 17.48 | 3.69 | 10.89 |

| 순위 | 파일:function | self µs/request | inclusive µs/request |
|---:|---|---:|---:|
| 1 | libc.so:_int_malloc | 1.0765 | 1.2853 |
| 2 | 미해결 stack symbol | 0.8033 | 15.9541 |
| 3 | framework/bench/grpc/cpp/client/bench_cpp_client.cpp:framework_driver_t::run_tasks<BenchPayload> | 0.6266 | 12.6926 |
| 4 | libc.so:__memmove_avx_unaligned_erms | 0.5302 | 0.5302 |
| 5 | libstdc++.so:_Sp_counted_base::_M_release | 0.4981 | 0.5623 |
| 6 | libc.so:malloc | 0.4338 | 1.7191 |
| 7 | kernel:__schedule | 0.4177 | 0.4177 |
| 8 | libc.so:pthread_mutex_lock | 0.4177 | 0.4177 |
| 9 | libstdc++.so:std::__shared_count::__shared_count | 0.3213 | 0.3213 |
| 10 | kernel:_raw_write_unlock_irq | 0.3053 | 0.3053 |
| 11 | framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:header_writer_t::append | 0.3053 | 0.3053 |
| 12 | libc.so:pthread_mutex_unlock | 0.2410 | 0.2410 |
| 13 | libc.so:_int_free_merge_chunk | 0.2410 | 0.2571 |
| 14 | framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:client_call_codec_t::create_envelope | 0.2249 | 0.2892 |
| 15 | kernel:_raw_spin_unlock_irqrestore | 0.2249 | 0.2249 |

인코딩은 baseline 2.15 µs 중 1.34 µs를 줄여야 하며, header와 envelope 함수 외에도 payload protobuf 및 service-wire 작업이 이 단계에 들어간다.

등록은 4.42 µs 중 2.76 µs를 줄여야 하며, operation registry, admission/deadline, raw request owner turn과 lane 처리가 관측된다.

결과 관찰은 4.25 µs 중 2.65 µs를 줄여야 하며, task/shared-state와 receiver completion, benchmark의 collect/validate 경로가 포함된다.

binding 제출은 5.54 µs 중 3.45 µs를 줄여야 하며, Framework caller에서 C++ binding, Core, kernel transport까지 실행된 CPU가 가장 큰 부분이다.

기타는 1.11 µs 중 0.69 µs를 줄여야 하며, 분류된 소유 함수를 찾지 못한 비용과 benchmark loop, 일부 allocator·동기화 작업이 남는다.

감축량은 총 17.48−6.59=10.89 µs를 다섯 부분의 기준 비용에 비례 배분한 값이며, 각 부분을 동일하게 62.3% 줄이는 산식이다. 이는 예상 성능이 아니라 목표를 만족시키는 산술 분배다. 각 함수의 inclusive 값은 서로 겹치므로 합산하지 않는다.

stage 분류는 leaf에서 caller 방향으로 처음 일치하는 소유 함수에 표본을 하나만 배정했다. 인코딩은 protobuf/generated payload, envelope/client-call codec, header 및 service-wire codec으로 분류했다. 등록은 operation owner/request turn, registry, admission, deadline과 그 Framework 경로로 분류했다. 결과 관찰은 task/coroutine/shared-state, receiver, completion 및 benchmark collect/validate로 분류했다. binding 제출은 binding/Core 및 kernel transport 함수로 분류했다. allocator와 lock/scheduler 프레임은 더 바깥의 요청 소유 함수가 확인되면 그 단계에 귀속했고, 소유자를 찾지 못한 비용은 기타로 두었다.

프로파일 빌드는 `/root/jobs/fx/cx/v21/s033/ccall/build`에서 Release, `-g`, `-fno-omit-frame-pointer`로 만들었다. 서버 affinity는 CPUs 8–15, client affinity는 CPUs 0–7이었으며 profile snapshot에서도 source와 target affinity가 각각 이 범위로 확인됐다. Framework source caller TID 2970734는 profile 동안 0.883 core, 19.682 µs/request였고 binding caller TID 2972256은 1.013 core, 3.685 µs/request였다. Framework caller 표본은 1,225개다. profile capture의 Framework 처리율은 44,842 req/s, binding은 274,950 req/s였다.

무프로파일 결과는 Framework 51,262 req/s, binding 261,096 req/s였다. 제공된 ratio044 기준 51,942 / 253,036 req/s 대비 각각 −1.3% / +3.2%로 빌드 재현성은 근접했다. 각 무프로파일 실행과 프로파일 실행 모두 completed와 received 수가 같았고 errors와 abandoned는 0이었다. profile raw 파일은 모두 1.1 MB 미만이었다. MemAvailable은 실행 전·profile capture 때 12 GiB 이상이었다.

profile caller 전체는 Framework 19.682 µs/request로 baseline 17.48보다 12.6% 높았다. 이에 따라 첫 표의 Framework 부분은 프로파일 단계 비율을 제공된 17.48 µs에 정규화했고, 위 함수표는 profile에서 직접 얻은 값이다. binding profile 전체는 3.685 µs/request였다. 가장 큰 inclusive 항목인 미해결 symbol은 self 0.8033 µs, inclusive 15.9541 µs로 stack attribution의 한계다. 이 미해결 프레임은 별도 단계 비용으로 더하지 않았다.

코드 변경 없음.
