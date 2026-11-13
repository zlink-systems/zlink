# Framework 네 언어의 동일 세션 측정

Framework÷binding 목표는 0.6으로 유지한다. 1,024 B의 send-saturation, request-backpressure, request-serial을 같은 배타 lock 세션에서 측정했다. 기준 언어는 11-01 표에 따라 각각 .NET, Node, Java로 고정했다. 이번 세션의 최고값으로 기준을 바꾸지 않았다.

처리량의 역수 µs/msg는 처리 능력을 비교하는 값이다. 두 프로세스의 user+kernel CPU µs/msg와 다르다. request의 한 메시지는 request/reply 왕복 한 건이며, send는 target에 전달된 command 한 건이다. F는 Framework, B는 binding을 뜻한다.

무제한 median에서 C++ Framework는 send 184,781 msg/s, backpressure 52,802 req/s, serial 8,001 req/s였다. 기준의 Framework 계층 시간과 비교한 차이는 각각 +2.861, −3.697, +23.119 µs/msg다. request-backpressure의 음수는 고정 기준인 Node보다 자기 F−B의 절대 시간이 작다는 뜻이다. F÷B 비율과 절대 F−B 시간의 순위는 binding 처리 능력이 달라 같지 않다.

해석 범위는 측정값과 코드로 확인한 단계까지다. 역할 CPU의 other가 남아 있어 C++·Java send-saturation과 .NET request-serial의 세부 병목 및 추가 논리 단계를 확정하지 못했다. 처리량 역수의 차이를 paced CPU로 완전히 분해한 결과로 해석하지 않는다.

## 무제한 처리량

값은 RUNS=3의 median과 min–max다. request 셀의 단위는 req/s, send 셀은 msg/s다. µs/msg의 범위는 처리량 범위를 역수로 바꿨다.

| 셀 | 언어 | Framework 처리량 | binding 처리량 | Framework µs/msg | binding µs/msg | F÷B |
|---|---|---:|---:|---:|---:|---:|
| send-saturation | C++ | 184,781 (184,587–185,750) | 594,110 (575,250–627,297) | 5.412 (5.384–5.418) | 1.683 (1.594–1.738) | 0.311 |
| send-saturation | Java | 113,344 (111,933–114,699) | 315,194 (311,694–320,058) | 8.823 (8.718–8.934) | 3.173 (3.124–3.208) | 0.360 |
| send-saturation | .NET **기준** | 195,215 (189,951–199,290) | 235,046 (228,670–237,908) | 5.123 (5.018–5.265) | 4.254 (4.203–4.373) | 0.831 |
| send-saturation | Node | 77,783 (74,241–79,028) | 113,531 (108,198–118,948) | 12.856 (12.654–13.470) | 8.808 (8.407–9.242) | 0.685 |
| request-backpressure | C++ | 52,802 (48,937–53,041) | 282,024 (274,806–282,805) | 18.939 (18.853–20.435) | 3.546 (3.536–3.639) | 0.187 |
| request-backpressure | Java | 58,528 (54,233–60,258) | 124,874 (122,178–127,765) | 17.086 (16.595–18.439) | 8.008 (7.827–8.185) | 0.469 |
| request-backpressure | .NET | 36,963 (36,385–38,248) | 61,748 (59,301–62,208) | 27.054 (26.145–27.484) | 16.195 (16.075–16.863) | 0.599 |
| request-backpressure | Node **기준** | 18,189 (17,766–18,892) | 27,863 (27,805–28,057) | 54.979 (52.932–56.286) | 35.890 (35.641–35.965) | 0.653 |
| request-serial | C++ | 8,001 (7,889–8,276) | 16,771 (14,962–17,078) | 124.979 (120.826–126.764) | 59.628 (58.556–66.836) | 0.477 |
| request-serial | Java **기준** | 9,223 (9,026–9,444) | 15,107 (14,904–15,108) | 108.426 (105.888–110.794) | 66.194 (66.192–67.096) | 0.610 |
| request-serial | .NET | 7,059 (6,901–7,135) | 11,526 (11,288–11,730) | 141.661 (140.162–144.911) | 86.763 (85.252–88.590) | 0.612 |
| request-serial | Node | 5,398 (5,286–5,554) | 8,897 (8,806–9,649) | 185.250 (180.044–189.165) | 112.400 (103.638–113.556) | 0.607 |

## send-saturation 1 KiB

공통 pacing 목표는 29,000 msg/s다. 기준 언어는 .NET이다. CPU 행은 두 프로세스 합계이며, Core I/O 행은 thread schedstat 실측값이다. 나머지 역할 행은 profile의 표본 비율을 같은 rate의 나머지 CPU에 배분한 추정이다. 역할이 같은 OS thread를 공유해도 CPU를 중복 합산하지 않았다. 표본에 잡히지 않은 역할의 0.000은 해당 단계가 없다는 증거가 아니다. 함수 inlining과 stack 해석 한계로 분류하지 못한 CPU는 other에 남겼다. other는 총 CPU에서 빠진 값이 아니라 활동별 소유를 확정하지 못한 값이다.

| 측정값 | C++ F | Java F | .NET F **기준** | Node F | C++ B | Java B | .NET B **기준** | Node B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 무제한 µs/msg | 5.412 | 8.823 | 5.123 | 12.856 | 1.683 | 3.173 | 4.254 | 8.808 |
| paced 실제 msg/s | 29,000.6 | 29,000.3 | 29,000.4 | 29,001.6 | 29,000.8 | 29,001.1 | 29,000.5 | 29,001.3 |
| 총 CPU µs/msg | 26.786 | 43.548 | 44.033 | 42.781 | 11.845 | 29.662 | 40.355 | 56.182 |
| source CPU µs/msg | 11.157 | 23.474 | 27.446 | 20.414 | 6.070 | 20.245 | 33.539 | 15.037 |
| target CPU µs/msg | 15.630 | 20.073 | 16.588 | 22.367 | 5.775 | 9.417 | 6.816 | 41.144 |
| user CPU µs/msg | 9.891 | 39.447 | 36.844 | 28.594 | 3.183 | 25.144 | 30.591 | 41.255 |
| kernel CPU µs/msg | 16.895 | 4.101 | 7.190 | 14.187 | 8.663 | 4.518 | 9.763 | 14.927 |
| caller/submit CPU µs/msg | 0.000 | 2.325 | 2.690 | 3.001 | 0.000 | 5.789 | 0.617 | 1.352 |
| dispatch/mesh CPU µs/msg | 9.772 | 4.427 | 17.770 | 10.907 | 0.299 | 1.054 | 0.548 | 0.384 |
| handler CPU µs/msg | 0.482 | 0.000 | 1.236 | 0.260 | 0.000 | 0.794 | 1.053 | 15.703 |
| Core I/O send CPU µs/msg | 5.418 | 9.809 | 5.321 | 9.220 | 3.269 | 6.350 | 4.378 | 6.821 |
| Core I/O receive CPU µs/msg | 4.246 | 7.171 | 3.727 | 7.305 | 3.003 | 5.156 | 3.728 | 5.921 |
| completion CPU µs/msg | 0.000 | 1.550 | 0.627 | 3.385 | 0.000 | 1.971 | 2.284 | 2.615 |
| binding receive/poll CPU µs/msg | 0.000 | 0.000 | 1.116 | 0.000 | 0.000 | 0.000 | 0.542 | 13.140 |
| codec/copy CPU µs/msg | 0.203 | 3.119 | 0.674 | 5.615 | 0.448 | 2.940 | 0.399 | 4.623 |
| allocation CPU µs/msg | 0.031 | 1.929 | 1.221 | 0.622 | 0.048 | 1.051 | 0.571 | 0.917 |
| GC CPU µs/msg | 0.000 | 1.329 | 0.000 | 0.476 | 0.000 | 0.768 | 0.000 | 3.054 |
| pacing CPU µs/msg | 0.000 | 0.013 | 2.942 | 0.201 | 0.000 | 0.235 | 8.391 | 0.071 |
| other CPU µs/msg | 6.586 | 11.864 | 5.952 | 1.755 | 4.731 | 3.529 | 17.711 | 1.559 |
| Core I/O other CPU µs/msg | 0.036 | 0.022 | 0.714 | 0.026 | 0.059 | 0.027 | 0.021 | 0.024 |
| unobserved threads/tick residual CPU µs/msg | 0.011 | -0.010 | 0.044 | 0.007 | -0.011 | -0.003 | 0.112 | -0.001 |
| OS context switches/msg | 0.481 | 2.422 | 0.885 | 0.580 | 0.270 | 1.110 | 0.794 | 0.279 |
| thread wake 시작/msg (handoff 대용값) | 0.346 | 2.211 | 0.654 | 0.598 | 0.166 | 0.937 | 0.297 | 0.273 |
| 동일 프로세스 thread wakeup/msg | 0.211 | 1.844 | 0.530 | 0.220 | 0.107 | 0.803 | 0.201 | 0.085 |
| 프로세스 간 thread wakeup/msg | 0.135 | 0.367 | 0.124 | 0.378 | 0.059 | 0.134 | 0.096 | 0.189 |
| 프로세스 소유 미확인 wake 시작/msg | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| 관리 heap allocation bytes/msg | — | 7,694.4 | 5,738.9 | 12,776.0 추정 | — | 5,864.8 | 3,346.0 | 8,900.8 추정 |
| 관리 heap allocation count/msg | — | 159.163 | 50.089 추정 | 208.319 추정 | — | 46.214 | 24.294 추정 | 105.480 추정 |
| native malloc bytes/msg | 25,386.1 | 17,279.2 | 7,474.2 | 20,911.2 | 7,289.6 | 5,473.9 | 3,300.7 | 10,420.7 |
| native malloc calls/msg | 35.611 | 323.227 | 3.944 | 44.992 | 8.529 | 96.339 | 2.686 | 37.844 |
| payload-size copy/msg (코드 경로) | 3 | 3 | 3 | 4 | 3 | 4 | 2 | 2 |
| TCP data packets/msg (loopback capture) | 0.318 | 0.707 | 0.211 | 0.812 | 0.104 | 0.270 | 0.219 | 0.539 |
| TCP payload bytes/msg | 1,389.0 | 1,384.0 | 1,375.0 | 1,343.0 | 1,244.0 | 1,244.0 | 1,244.0 | 1,244.0 |
| TCP data segments/msg (TCP_INFO) | 0.319 | 0.710 | 0.211 | 0.815 | 0.104 | 0.271 | 0.220 | 0.541 |
| 별도 profile 실행의 CPU µs/msg | 27.446 | 49.860 | 48.969 | 47.956 | 11.913 | 29.853 | 49.314 | 57.003 |
| profile native CPU 표본 수 | 3301 | 7158 | 7415 | 7428 | 1355 | 4217 | 7569 | 9041 |
| profile 관리 runtime 표본 수 | 0 | 534 | 2430 | 7020 | 0 | 199 | 783 | 8200 |
| 별도 allocation 실행의 실제 msg/s | 28,999.1 | 29,000.4 | 29,000.5 | 29,001.1 | 29,000.9 | 29,000.8 | 29,001.5 | 29,001.6 |

Node target JS thread의 CPU는 F 14.659, B 34.621 µs/msg다. binding target은 빈 receive에서도 setImmediate poll을 반복한다(node/zlink-raw-server/main.js:139). 이 bench 비용이 F−B CPU의 음수에 영향을 주므로, 그 음수를 Framework 제품 계층의 CPU 절감량으로 단정하지 않는다.

네 언어의 native malloc 행은 같은 LD_PRELOAD counter로 수집했다. 관리 heap과 native allocator의 요청은 서로 다른 단위이므로 한 allocation 합계로 합산하지 않았다. Java native malloc에는 JVMTI·JFR 실행을 지원하는 JVM 비용도 포함된다. 관리 runtime의 native malloc을 비계측 제품 경로의 count로 단정하지 않았다.

### .NET 기준의 Framework 계층 차이

| 언어 | 자기 F−B µs/msg | 기준 F−B와의 차이 µs/msg | paced CPU의 F−B µs/msg | 기준의 paced CPU F−B와의 차이 µs/msg |
|---|---:|---:|---:|---:|
| C++ | +3.729 | +2.861 | +14.941 | +11.262 |
| Java | +5.650 | +4.782 | +13.886 | +10.207 |
| .NET 기준 | +0.868 | +0.000 | +3.679 | +0.000 |
| Node | +4.048 | +3.180 | -13.401 | -17.079 |

다음 값은 각 CPU 행에 `자기(F−B) − 기준(F−B)`를 적용한 차이다. 처리량 역수 차이를 이 CPU 값의 합으로 설명하지 않았다. 여러 thread가 병렬로 소비하는 CPU와 대기 시간이 처리량의 역수에 그대로 더해지지 않기 때문이다.

C++의 차이가 큰 CPU 행은 other +13.616 µs/msg, dispatch/mesh -7.749 µs/msg, pacing +5.448 µs/msg, caller/submit -2.073 µs/msg, completion +1.657 µs/msg.

Java의 차이가 큰 CPU 행은 other +20.094 µs/msg, dispatch/mesh -13.849 µs/msg, caller/submit -5.537 µs/msg, pacing +5.226 µs/msg, Core I/O send +2.516 µs/msg.

Node의 차이가 큰 CPU 행은 handler -15.626 µs/msg, binding receive/poll -13.714 µs/msg, other +11.956 µs/msg, dispatch/mesh -6.699 µs/msg, pacing +5.579 µs/msg.


확인한 추가 단계는 관련 CPU 행 차이의 절댓값이 큰 묶음부터 적었다. 묶음의 시간은 해당 단계만의 독립 비용이 아니다. allocation·copy·wake 시작을 각각 몇 µs라고 환산하지 않았다.

C++의 F/B body pass는 3/3회이며, 기준의 3/2회에 대한 계층 차이는 -1회/msg다. TCP data packet의 같은 계층 차이는 +0.223 packets/msg다.
C++의 경우 이 셀의 코드 대조에서 기준에 없는 추가 논리 단계를 확정하지 못했다. 위 CPU 행은 공통 단계의 구현 비용 차이와 bench 비용을 포함한다.
C++ send의 이미 완료된 task는 bench_async.hpp:46의 fast path에서 별도 completed-slot deque를 거치지 않는다. request용 queue 경로를 send 비용의 원인으로 단정하지 않았다.

Java의 F/B body pass는 3/4회이며, 기준의 3/2회에 대한 계층 차이는 -2회/msg다. TCP data packet의 같은 계층 차이는 +0.447 packets/msg다.
Java의 경우 이 셀의 코드 대조에서 기준에 없는 추가 논리 단계를 확정하지 못했다. 위 CPU 행은 공통 단계의 구현 비용 차이와 bench 비용을 포함한다.

Node의 F/B body pass는 4/2회이며, 기준의 3/2회에 대한 계층 차이는 +1회/msg다. TCP data packet의 같은 계층 차이는 +0.282 packets/msg다.
Node 추가 단계 1: Node는 protobuf 결과와 application frame을 JS Buffer에 만든 뒤 binding Message.from으로 독립 native 저장소를 만든다. 기준 .NET의 codec은 native Message span에 직접 쓴다. Node의 별도 native materialization은 framework/src/runtime/backend/node/node-raw-binding-port.ts:498 → bindings/node/src/zlink/contracts/messaging/message.ts:105에 있고, .NET 직접 쓰기는 ZLinkProtobufCodec.cs:100에 있다. Node의 payload.data()는 receive에서도 복사하지만 .NET의 protobuf 소유 bytes decode도 복사하므로 이 두 작업을 각각 추가 1회로 중복 계산하지 않았다. 관련 CPU 행 caller/submit, dispatch/mesh, codec/copy, allocation의 합계 차이는 -7.349 µs/msg다.


## request-backpressure 1 KiB

공통 pacing 목표는 7,000 msg/s다. 기준 언어는 Node이다. CPU 행은 두 프로세스 합계이며, Core I/O 행은 thread schedstat 실측값이다. 나머지 역할 행은 profile의 표본 비율을 같은 rate의 나머지 CPU에 배분한 추정이다. 역할이 같은 OS thread를 공유해도 CPU를 중복 합산하지 않았다. 표본에 잡히지 않은 역할의 0.000은 해당 단계가 없다는 증거가 아니다. 함수 inlining과 stack 해석 한계로 분류하지 못한 CPU는 other에 남겼다. other는 총 CPU에서 빠진 값이 아니라 활동별 소유를 확정하지 못한 값이다.

| 측정값 | C++ F | Java F | .NET F | Node F **기준** | C++ B | Java B | .NET B | Node B **기준** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 무제한 µs/msg | 18.939 | 17.086 | 27.054 | 54.979 | 3.546 | 8.008 | 16.195 | 35.890 |
| paced 실제 msg/s | 6,999.9 | 7,000.2 | 7,000.1 | 6,999.7 | 7,000.8 | 7,000.0 | 7,000.1 | 7,000.0 |
| 총 CPU µs/msg | 121.952 | 109.991 | 157.141 | 141.576 | 50.436 | 68.805 | 122.228 | 249.320 |
| source CPU µs/msg | 71.003 | 66.493 | 106.152 | 81.402 | 26.109 | 34.759 | 82.639 | 70.500 |
| target CPU µs/msg | 50.949 | 43.498 | 50.990 | 60.173 | 24.327 | 34.046 | 39.589 | 178.820 |
| user CPU µs/msg | 35.527 | 103.785 | 119.942 | 99.831 | 12.724 | 64.276 | 99.431 | 176.021 |
| kernel CPU µs/msg | 86.425 | 6.207 | 37.199 | 41.745 | 37.713 | 4.529 | 22.797 | 73.300 |
| caller/submit CPU µs/msg | 0.000 | 1.702 | 27.532 | 7.293 | 0.000 | 2.856 | 7.509 | 6.934 |
| dispatch/mesh CPU µs/msg | 42.482 | 10.419 | 20.158 | 29.072 | 1.165 | 2.397 | 2.534 | 3.784 |
| handler CPU µs/msg | 1.865 | 0.052 | 2.924 | 0.582 | 0.000 | 2.029 | 3.190 | 30.955 |
| Core I/O send CPU µs/msg | 24.786 | 19.898 | 24.377 | 30.055 | 18.040 | 23.457 | 29.262 | 36.482 |
| Core I/O receive CPU µs/msg | 21.311 | 18.162 | 20.642 | 21.214 | 14.079 | 20.178 | 25.368 | 29.816 |
| completion CPU µs/msg | 6.926 | 6.150 | 20.715 | 24.095 | 0.000 | 4.093 | 1.149 | 13.362 |
| binding receive/poll CPU µs/msg | 0.000 | 0.000 | 1.523 | 0.000 | 0.000 | 0.000 | 2.436 | 79.845 |
| codec/copy CPU µs/msg | 0.361 | 14.608 | 1.180 | 20.147 | 1.641 | 3.286 | 1.375 | 11.214 |
| allocation CPU µs/msg | 0.503 | 3.586 | 2.165 | 2.045 | 0.265 | 1.525 | 1.809 | 5.375 |
| GC CPU µs/msg | 0.000 | 2.755 | 0.000 | 1.117 | 0.000 | 1.544 | 0.000 | 9.187 |
| pacing CPU µs/msg | 0.000 | 0.213 | 4.232 | 0.251 | 0.000 | 0.051 | 5.384 | 0.785 |
| other CPU µs/msg | 23.600 | 32.371 | 25.602 | 5.626 | 14.955 | 7.381 | 41.749 | 21.437 |
| Core I/O other CPU µs/msg | 0.123 | 0.076 | 5.914 | 0.071 | 0.231 | 0.080 | 0.078 | 0.063 |
| unobserved threads/tick residual CPU µs/msg | -0.006 | -0.001 | 0.177 | 0.008 | 0.061 | -0.072 | 0.384 | 0.080 |
| OS context switches/msg | 3.753 | 2.719 | 3.915 | 3.187 | 1.272 | 1.168 | 2.011 | 3.379 |
| thread wake 시작/msg (handoff 대용값) | 3.418 | 2.538 | 3.750 | 3.313 | 0.819 | 1.179 | 1.895 | 3.595 |
| 동일 프로세스 thread wakeup/msg | 2.720 | 2.158 | 3.138 | 1.952 | 0.487 | 0.657 | 1.255 | 1.819 |
| 프로세스 간 thread wakeup/msg | 0.697 | 0.379 | 0.612 | 1.361 | 0.331 | 0.522 | 0.641 | 1.776 |
| 프로세스 소유 미확인 wake 시작/msg | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| 관리 heap allocation bytes/msg | — | 16,386.3 | 11,880.9 | 28,234.5 추정 | — | 10,606.9 | 4,573.8 | 31,257.2 추정 |
| 관리 heap allocation count/msg | — | 342.479 | 152.669 추정 | 443.598 추정 | — | 92.547 | 16.387 추정 | 309.460 추정 |
| native malloc bytes/msg | 50,773.1 | 36,070.2 | 21,886.0 | 35,617.5 | 17,996.6 | 22,901.3 | 13,941.3 | 17,239.7 |
| native malloc calls/msg | 63.245 | 698.498 | 11.815 | 92.571 | 19.766 | 195.289 | 8.313 | 86.088 |
| payload-size copy/msg (코드 경로) | 7 | 6 | 6 | 8 | 7 | 8 | 4 | 4 |
| TCP data packets/msg (loopback capture) | 1.369 | 0.828 | 1.511 | 1.563 | 0.551 | 0.967 | 1.230 | 1.978 |
| TCP payload bytes/msg | 2,842.1 | 2,878.0 | 2,814.9 | 2,771.1 | 2,503.3 | 2,504.0 | 2,504.0 | 2,504.0 |
| TCP data segments/msg (TCP_INFO) | 1.369 | 0.831 | 1.512 | 1.565 | 0.552 | 0.969 | 1.230 | 1.981 |
| 별도 profile 실행의 CPU µs/msg | 116.266 | 115.260 | 193.424 | 165.090 | 51.574 | 67.795 | 107.566 | 264.393 |
| profile native CPU 표본 수 | 3594 | 3927 | 7016 | 6020 | 1438 | 2338 | 3859 | 9901 |
| profile 관리 runtime 표본 수 | 0 | 639 | 2252 | 6865 | 0 | 128 | 266 | 7546 |
| 별도 allocation 실행의 실제 msg/s | 7,000.0 | 7,000.1 | 7,000.1 | 7,000.0 | 7,002.0 | 6,999.7 | 6,999.7 | 7,000.4 |

Node target JS thread의 CPU는 F 36.361, B 143.067 µs/msg다. binding target은 빈 receive에서도 setImmediate poll을 반복한다(node/zlink-raw-server/main.js:139). 이 bench 비용이 F−B CPU의 음수에 영향을 주므로, 그 음수를 Framework 제품 계층의 CPU 절감량으로 단정하지 않는다.

네 언어의 native malloc 행은 같은 LD_PRELOAD counter로 수집했다. 관리 heap과 native allocator의 요청은 서로 다른 단위이므로 한 allocation 합계로 합산하지 않았다. Java native malloc에는 JVMTI·JFR 실행을 지원하는 JVM 비용도 포함된다. 관리 runtime의 native malloc을 비계측 제품 경로의 count로 단정하지 않았다.

### Node 기준의 Framework 계층 차이

| 언어 | 자기 F−B µs/msg | 기준 F−B와의 차이 µs/msg | paced CPU의 F−B µs/msg | 기준의 paced CPU F−B와의 차이 µs/msg |
|---|---:|---:|---:|---:|
| C++ | +15.393 | -3.697 | +71.516 | +179.261 |
| Java | +9.078 | -10.012 | +41.186 | +148.931 |
| .NET | +10.859 | -8.230 | +34.913 | +142.658 |
| Node 기준 | +19.089 | +0.000 | -107.745 | +0.000 |

다음 값은 각 CPU 행에 `자기(F−B) − 기준(F−B)`를 적용한 차이다. 처리량 역수 차이를 이 CPU 값의 합으로 설명하지 않았다. 여러 thread가 병렬로 소비하는 CPU와 대기 시간이 처리량의 역수에 그대로 더해지지 않기 때문이다.

C++의 차이가 큰 CPU 행은 binding receive/poll +79.845 µs/msg, handler +32.239 µs/msg, other +24.456 µs/msg, dispatch/mesh +16.030 µs/msg, Core I/O receive +15.834 µs/msg.

Java의 차이가 큰 CPU 행은 binding receive/poll +79.845 µs/msg, other +40.801 µs/msg, handler +28.397 µs/msg, dispatch/mesh -17.265 µs/msg, GC +9.282 µs/msg.

.NET의 차이가 큰 CPU 행은 binding receive/poll +78.932 µs/msg, handler +30.108 µs/msg, caller/submit +19.665 µs/msg, codec/copy -9.128 µs/msg, completion +8.832 µs/msg.


확인한 추가 단계는 관련 CPU 행 차이의 절댓값이 큰 묶음부터 적었다. 묶음의 시간은 해당 단계만의 독립 비용이 아니다. allocation·copy·wake 시작을 각각 몇 µs라고 환산하지 않았다.

C++의 F/B body pass는 7/7회이며, 기준의 8/4회에 대한 계층 차이는 -4회/msg다. TCP data packet의 같은 계층 차이는 +1.233 packets/msg다.
C++ 추가 단계 1: C++ echo handler는 const request를 값으로 반환해 body를 1회 복사한다(cpp/framework/bench_framework_cpp_server.cpp:26). 기준 Node handler는 같은 request 객체를 반환한다(node/zlink-framework-server/main.js:37). C++ binding echo에도 같은 body 소유 복사가 있으므로 이 한 단계 전체를 F−B 차이에 다시 더하지 않았다. 관련 CPU 행 handler, codec/copy의 합계 차이는 +22.026 µs/msg다.
C++ 추가 단계 2: C++ bench는 public task 완료를 completion_signal_t의 completed-slot deque에 넣은 뒤 caller가 별도로 drain한다. Node bench는 Promise 완료에서 metrics를 갱신하므로 이 별도 deque가 없다. framework/bench/grpc/cpp/common/bench_async.hpp:54,63과 cpp/client/bench_cpp_client.cpp:943,970을 Node client/bench-core.js:356과 대조했다. 이는 bench 단계이며 제품 runtime의 추가 단계로 옮겨 해석하지 않았다. 관련 CPU 행 caller/submit, completion의 합계 차이는 -4.166 µs/msg다.

Java의 F/B body pass는 6/8회이며, 기준의 8/4회에 대한 계층 차이는 -6회/msg다. TCP data packet의 같은 계층 차이는 +0.277 packets/msg다.
Java 추가 단계 1: Java Framework completion은 원래 turn 밖에서 ZLinkServiceCompletionDispatcher.post의 ForkJoinPool.commonPool().execute로 process pool turn을 시작한다. 기준 Node는 binding reply Promise의 continuation에서 처리해 Framework가 별도 process pool에 post하는 단계가 없다. Java runtime/internal/service/ZLinkServiceCompletionDispatcher.java:25,51과 Node runtime/backend/node/node-raw-binding-port.ts:596,632를 대조했다. 이미 completion turn인 경로의 Java inline 재사용은 추가 post로 세지 않았다. 관련 CPU 행 dispatch/mesh, completion의 합계 차이는 -25.942 µs/msg다.

.NET의 F/B body pass는 6/4회이며, 기준의 8/4회에 대한 계층 차이는 -2회/msg다. TCP data packet의 같은 계층 차이는 +0.697 packets/msg다.
.NET 추가 단계 1: .NET Framework의 ZLinkCompletionDispatcher는 ThreadPool.UnsafeQueueUserWorkItem으로 completion work를 별도 pool turn에 보낸다(Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:16). 기준 Node의 requestReply/observeRequestReply는 JS Promise continuation을 사용한다(node-raw-binding-port.ts:596,632). 이는 native Core I/O에서 managed callback으로 넘어오는 공통 경로와 구분한 Framework pool post 단계다. 관련 CPU 행 dispatch/mesh, completion의 합계 차이는 +1.169 µs/msg다.


## request-serial 1 KiB

공통 pacing 목표는 2,100 msg/s다. 기준 언어는 Java이다. CPU 행은 두 프로세스 합계이며, Core I/O 행은 thread schedstat 실측값이다. 나머지 역할 행은 profile의 표본 비율을 같은 rate의 나머지 CPU에 배분한 추정이다. 역할이 같은 OS thread를 공유해도 CPU를 중복 합산하지 않았다. 표본에 잡히지 않은 역할의 0.000은 해당 단계가 없다는 증거가 아니다. 함수 inlining과 stack 해석 한계로 분류하지 못한 CPU는 other에 남겼다. other는 총 CPU에서 빠진 값이 아니라 활동별 소유를 확정하지 못한 값이다.

| 측정값 | C++ F | Java F **기준** | .NET F | Node F | C++ B | Java B **기준** | .NET B | Node B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 무제한 µs/msg | 124.979 | 108.426 | 141.661 | 185.250 | 59.628 | 66.194 | 86.763 | 112.400 |
| paced 실제 msg/s | 2,099.9 | 2,100.0 | 2,100.0 | 2,100.0 | 2,100.0 | 2,100.1 | 2,100.0 | 2,100.0 |
| 총 CPU µs/msg | 264.556 | 258.707 | 351.472 | 268.128 | 173.909 | 185.581 | 250.089 | 623.016 |
| source CPU µs/msg | 148.144 | 147.590 | 251.052 | 153.919 | 86.700 | 100.763 | 191.893 | 102.167 |
| target CPU µs/msg | 116.411 | 111.117 | 100.421 | 114.209 | 87.209 | 84.818 | 58.196 | 520.849 |
| user CPU µs/msg | 106.060 | 242.930 | 284.808 | 237.752 | 49.034 | 175.742 | 199.019 | 540.197 |
| kernel CPU µs/msg | 158.496 | 15.777 | 66.664 | 30.377 | 124.875 | 9.839 | 51.070 | 82.820 |
| caller/submit CPU µs/msg | 0.000 | 1.700 | 69.079 | 7.879 | 0.000 | 11.547 | 37.060 | 11.006 |
| dispatch/mesh CPU µs/msg | 95.526 | 36.625 | 44.349 | 62.430 | 6.797 | 11.721 | 5.665 | 9.212 |
| handler CPU µs/msg | 3.256 | 0.000 | 4.516 | 1.465 | 0.000 | 5.965 | 3.772 | 104.187 |
| Core I/O send CPU µs/msg | 60.412 | 60.680 | 52.522 | 55.302 | 65.352 | 59.967 | 42.434 | 44.859 |
| Core I/O receive CPU µs/msg | 43.975 | 53.996 | 36.313 | 37.998 | 46.906 | 59.404 | 30.229 | 40.584 |
| completion CPU µs/msg | 8.385 | 7.065 | 38.523 | 32.077 | 0.000 | 7.489 | 21.571 | 15.685 |
| binding receive/poll CPU µs/msg | 0.000 | 0.000 | 1.780 | 0.000 | 0.000 | 0.000 | 1.559 | 285.758 |
| codec/copy CPU µs/msg | 0.880 | 13.156 | 2.040 | 25.280 | 2.039 | 6.616 | 1.889 | 16.872 |
| allocation CPU µs/msg | 0.474 | 7.617 | 2.803 | 3.651 | 0.680 | 3.503 | 1.541 | 11.631 |
| GC CPU µs/msg | 0.000 | 4.371 | 0.000 | 0.917 | 0.000 | 0.359 | 0.000 | 9.392 |
| pacing CPU µs/msg | 0.000 | 0.100 | 10.666 | 30.540 | 0.000 | 0.190 | 25.520 | 18.670 |
| other CPU µs/msg | 51.487 | 72.998 | 75.056 | 10.546 | 51.735 | 16.906 | 77.510 | 54.857 |
| Core I/O other CPU µs/msg | 0.403 | 0.245 | 12.822 | 0.188 | 0.682 | 0.248 | 0.167 | 0.196 |
| unobserved threads/tick residual CPU µs/msg | -0.243 | 0.155 | 1.006 | -0.144 | -0.283 | 1.668 | 1.170 | 0.107 |
| OS context switches/msg | 10.959 | 10.595 | 12.867 | 8.522 | 7.473 | 6.543 | 8.524 | 5.769 |
| thread wake 시작/msg (handoff 대용값) | 9.049 | 10.029 | 12.376 | 7.973 | 5.998 | 6.002 | 7.980 | 5.220 |
| 동일 프로세스 thread wakeup/msg | 7.051 | 8.034 | 10.379 | 5.974 | 4.000 | 4.005 | 5.982 | 3.222 |
| 프로세스 간 thread wakeup/msg | 1.998 | 1.994 | 1.997 | 1.999 | 1.998 | 1.997 | 1.998 | 1.999 |
| 프로세스 소유 미확인 wake 시작/msg | 0.000 | 0.001 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| 관리 heap allocation bytes/msg | — | 17,313.5 | 13,087.4 | 30,395.2 추정 | — | 10,728.2 | 4,877.0 | 100,114.8 추정 |
| 관리 heap allocation count/msg | — | 363.819 | 173.209 추정 | 469.046 추정 | — | 96.213 | 37.650 추정 | 842.382 추정 |
| native malloc bytes/msg | 50,854.9 | 22,134.7 | 6,045.0 | 18,998.6 | 10,008.0 | 6,847.7 | 3,105.6 | 18,333.5 |
| native malloc calls/msg | 92.527 | 747.153 | 12.525 | 105.140 | 24.038 | 202.593 | 10.378 | 178.895 |
| payload-size copy/msg (코드 경로) | 7 | 6 | 6 | 8 | 7 | 8 | 4 | 4 |
| TCP data packets/msg (loopback capture) | 2.000 | 2.000 | 2.000 | 2.000 | 2.000 | 2.000 | 2.000 | 2.000 |
| TCP payload bytes/msg | 2,840.3 | 2,878.0 | 2,813.4 | 2,770.9 | 2,504.0 | 2,504.0 | 2,504.1 | 2,503.9 |
| TCP data segments/msg (TCP_INFO) | 2.001 | 2.003 | 2.000 | 2.001 | 2.000 | 2.002 | 2.000 | 2.001 |
| 별도 profile 실행의 CPU µs/msg | 282.225 | 272.552 | 487.007 | 303.061 | 190.455 | 202.805 | 364.379 | 646.037 |
| profile native CPU 표본 수 | 2438 | 2649 | 5148 | 3204 | 1479 | 1999 | 3881 | 7253 |
| profile 관리 runtime 표본 수 | 0 | 362 | 1988 | 11847 | 0 | 34 | 449 | 20424 |
| 별도 allocation 실행의 실제 msg/s | 2,100.0 | 2,099.9 | 2,100.0 | 2,099.9 | 2,100.1 | 2,100.0 | 2,100.0 | 2,100.0 |

Node target JS thread의 CPU는 F 77.585, B 476.200 µs/msg다. binding target은 빈 receive에서도 setImmediate poll을 반복한다(node/zlink-raw-server/main.js:139). 이 bench 비용이 F−B CPU의 음수에 영향을 주므로, 그 음수를 Framework 제품 계층의 CPU 절감량으로 단정하지 않는다.

네 언어의 native malloc 행은 같은 LD_PRELOAD counter로 수집했다. 관리 heap과 native allocator의 요청은 서로 다른 단위이므로 한 allocation 합계로 합산하지 않았다. Java native malloc에는 JVMTI·JFR 실행을 지원하는 JVM 비용도 포함된다. 관리 runtime의 native malloc을 비계측 제품 경로의 count로 단정하지 않았다.

### Java 기준의 Framework 계층 차이

| 언어 | 자기 F−B µs/msg | 기준 F−B와의 차이 µs/msg | paced CPU의 F−B µs/msg | 기준의 paced CPU F−B와의 차이 µs/msg |
|---|---:|---:|---:|---:|
| C++ | +65.351 | +23.119 | +90.647 | +17.521 |
| Java 기준 | +42.232 | +0.000 | +73.126 | +0.000 |
| .NET | +54.898 | +12.666 | +101.383 | +28.257 |
| Node | +72.850 | +30.618 | -354.888 | -428.014 |

다음 값은 각 CPU 행에 `자기(F−B) − 기준(F−B)`를 적용한 차이다. 처리량 역수 차이를 이 CPU 값의 합으로 설명하지 않았다. 여러 thread가 병렬로 소비하는 CPU와 대기 시간이 처리량의 역수에 그대로 더해지지 않기 때문이다.

C++의 차이가 큰 CPU 행은 dispatch/mesh +63.825 µs/msg, other -56.340 µs/msg, caller/submit +9.847 µs/msg, handler +9.221 µs/msg, completion +8.809 µs/msg.

.NET의 차이가 큰 CPU 행은 other -58.546 µs/msg, caller/submit +41.866 µs/msg, completion +17.376 µs/msg, pacing -14.764 µs/msg, dispatch/mesh +13.780 µs/msg.

Node의 차이가 큰 CPU 행은 binding receive/poll -285.758 µs/msg, other -100.403 µs/msg, handler -96.756 µs/msg, dispatch/mesh +28.314 µs/msg, completion +16.816 µs/msg.


확인한 추가 단계는 관련 CPU 행 차이의 절댓값이 큰 묶음부터 적었다. 묶음의 시간은 해당 단계만의 독립 비용이 아니다. allocation·copy·wake 시작을 각각 몇 µs라고 환산하지 않았다.

C++의 F/B body pass는 7/7회이며, 기준의 6/8회에 대한 계층 차이는 +2회/msg다. TCP data packet의 같은 계층 차이는 +0.000 packets/msg다.
C++ 추가 단계 1: C++ bench의 task 완료 observer → completed-slot deque → caller drain은 기준 Java bench의 operation.await 경로에 없는 별도 bookkeeping이다(cpp/common/bench_async.hpp:54,63; cpp/client/bench_cpp_client.cpp:943,970; Java client/BenchDrivers.java:499). serial에서는 admission과 reply observer가 모두 이 signal을 사용할 수 있다. callback 등록 수와 실제 OS wake 수를 같다고 세지 않았다. 관련 CPU 행 caller/submit, completion의 합계 차이는 +18.656 µs/msg다.
C++ 추가 단계 2: C++ echo handler는 request body를 값으로 1회 복사하지만 기준 Java BenchEchoHandler는 같은 immutable request를 반환한다(cpp/framework/bench_framework_cpp_server.cpp:26; java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/BenchEchoHandler.java:37). C++ binding echo의 복사도 있어 자기 binding을 뺀 뒤의 copy 차이는 표의 전체 경로로 판단한다. 관련 CPU 행 handler, codec/copy의 합계 차이는 +1.522 µs/msg다.

.NET의 F/B body pass는 6/4회이며, 기준의 6/8회에 대한 계층 차이는 +4회/msg다. TCP data packet의 같은 계층 차이는 +0.000 packets/msg다.
.NET의 경우 이 셀의 코드 대조에서 기준에 없는 추가 논리 단계를 확정하지 못했다. 위 CPU 행은 공통 단계의 구현 비용 차이와 bench 비용을 포함한다.
.NET과 Java Framework의 body pass는 모두 왕복 6회다. 두 언어의 F−B copy 차이 +4회/msg는 Java binding의 ByteString.copyFrom·toByteArray·Message.from 경로와 .NET binding의 직접 span encode 차이에서 생긴다. Framework가 4회 더 복사한다는 뜻이 아니다.

Node의 F/B body pass는 8/4회이며, 기준의 6/8회에 대한 계층 차이는 +6회/msg다. TCP data packet의 같은 계층 차이는 +0.000 packets/msg다.
Node 추가 단계 1: Node는 각 방향에서 JS Buffer의 application frame을 binding Message.from으로 독립 native message에 옮긴다(node-raw-binding-port.ts:498; bindings/node/src/zlink/contracts/messaging/message.ts:105). 기준 Java serializer는 native Message의 ByteBuffer에 protobuf를 직접 쓴다(ZLinkProtobufMessageSerializer.java:38). Node F의 body pass는 왕복 8회, Java F는 6회다. Node payload.data()의 copy와 Java parser의 소유 bytes copy는 양쪽 receive 경로에 대응하므로 별도의 추가 단계 두 개로 중복 계산하지 않았다. 관련 CPU 행 caller/submit, dispatch/mesh, codec/copy, allocation의 합계 차이는 +24.806 µs/msg다.


## 경로 대조

1,024 B body 생성과 fill 뒤부터 수신 typed body까지의 사용자 공간 copy와 protobuf body write를 센다. kernel/Core transport copy와 metadata만의 copy는 제외한다. send는 편도, request는 왕복이다. view와 move는 0회다.

### C++

F: framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:30 — protobuf body를 native Message에 쓴다. 편도 1회다.

F: framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:551 → framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:3245 — native body를 application frame에 복사한다. 편도 1회다.

F: framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:3400 — received frame의 inner part는 external view다. body copy는 없다.

F: framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:41 — ParseFromArray가 protobuf bytes field를 소유한다. 편도 1회다.

F request: framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:26 — const request를 값으로 반환해 echo body를 한 번 복사한다.

B: framework/bench/grpc/cpp/client/bench_cpp_client.cpp:635 — encode_bench_payload와 Message.from의 body write 2회, target ParseFromArray 1회다.

B request: framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:62 — reply.set_body copy 1회, serialize와 native materialization 2회, source parse 1회가 더해진다.

### Java

F: framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java:110 — UnsafeByteOperations.unsafeWrap은 source body를 복사하지 않는다.

F: framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:38 — CodedOutputStream이 native Message에 protobuf body를 쓴다. 편도 1회다.

F: framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:383 — multipart frame에 body를 복사한다. 편도 1회다.

F: framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:97 — parser.parseFrom의 bytes field copy 1회다. multipart receive view 자체는 복사하지 않는다.

F request: framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/BenchEchoHandler.java:37 — 같은 immutable request를 반환한다. echo copy는 없다.

B: framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/RawWire.java:51 — ByteString.copyFrom, protobuf toByteArray, Message.from의 3회와 decode parse 1회다. request는 target reply도 같은 encode와 decode를 수행한다.

### .NET

F/B: framework/bench/grpc/dotnet/Shared/BenchMetricHeaders.cs:35 — UnsafeWrap으로 source body를 복사하지 않는다.

F: framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:100 — native Message span에 protobuf body를 쓴다. 편도 1회다.

F: framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkApplicationPayloadEnvelopeCodec.cs:407 — application frame에 part를 복사한다. 편도 1회다.

F: framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:7363 — receive는 multipart view다. body copy는 없다.

F: framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:160 — protobuf MergeFrom bytes field가 body를 소유한다. 편도 1회다.

F request: framework/bench/grpc/dotnet/ZLinkServer/Program.cs:72 — 같은 request를 반환한다. echo copy는 없다.

B: framework/bench/grpc/dotnet/Client/Program.cs:445 → EncodeRawPayload와 ZLinkRawServer/Program.cs:106 — raw protobuf를 native span에 직접 쓰므로 write 1회와 receive protobuf parse 1회다. Shared/RawWire.cs:7,9가 encode/decode를 소유한다.

### Node

F/B: framework/bench/grpc/node/shared/framework-protobuf.js:18, shared/raw-wire.js:39 — protobuf requestSerialize가 body를 쓴다. 편도 1회다.

F: framework/languages/node/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts:269 — application frame에 body part를 쓴다. 편도 1회다.

F: framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts:498 — Buffer를 binding operation.message에 전달한다. bindings/node/src/zlink/contracts/messaging/message.ts:105의 독립 native copy 1회를 센다.

F: framework/languages/node/packages/framework-codec-protobuf/src/framework.ts:92 → contracts/Common/ZLinkEncodedPayload.ts:23 — serializer가 payload.data()를 호출해 body를 한 번 복사한다. protobufjs decode의 body view 자체는 copy로 세지 않는다.

F request: framework/bench/grpc/node/zlink-framework-server/main.js:37 — handler는 같은 request를 반환한다. 별도 echo body copy는 없다.

B: framework/bench/grpc/node/shared/raw-wire.js:39 — requestSerialize와 Message.from의 2회다. request reply도 같은 2회다.

## 측정 방법

워크트리는 `/root/worktree/zlink-xlang`이다. main 스냅샷 `d961e51e231234e8404d74164d2a27c0cf2fce41`의 detached HEAD와 기존 CPU pin 및 bench 계측 변경에서 실행했다. Core·binding·Framework 제품 소스와 spec은 수정하지 않았다. git은 status와 diff만 사용했고, metadata의 git 호출은 `bin/git` shim으로 막았다. C++ 병렬 작업의 worktree는 수정하지 않았다. 추가 pacing 변경 파일은 framework/bench/grpc 기준 cpp/client/bench_cpp_client.cpp, java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java, dotnet/Client/BenchMeasurement.cs, node/client/bench-core.js다. 네 run_local.sh의 기존 CPU pin 변경은 보존했다. diff는 [instrumentation.diff](instrumentation.diff)에 있다.

기존 `build-*.log`는 비어 있었고 이 worktree의 예상 실행 파일이 없어 네 언어의 누락 빌드를 실행했다. C++ 빌드는 `cpp-build-slot.sh`를 바깥 flock 없이 사용했고, Java/.NET은 `measure-shared.sh taskset -c 0-7 bash <lang>/build.sh`를 사용했다. Node도 공용 shared build wrapper를 사용했다. 빌드 로그와 실행 파일의 provenance는 같은 폴더의 로그와 manifest에 남겼다.

측정은 `flock -o /root/jobs/fx/cx/measure.lock python3 tools/session.py` 한 세션에서 수행했다. source는 CPU 0–7, target은 8–15로 고정했다. 무제한 측정은 warmup 10초와 active 10초를 3회 실행했다. 언어 순서와 F/B 순서는 두 번째 반복에서 뒤집었다. 공통 rate는 셀 내 24개 무제한 값의 최솟값의 0.4배를 send·backpressure는 1,000, serial은 100 단위로 내림해 정했다. 이는 목표 0.6의 변경이 아니라 CPU 비교를 위한 부하 설정이다. paced와 profile은 active 30초, allocation은 active 20초다. thread handoff의 wake 시작은 warmup 5초·active 15초의 별도 동일 rate 실행에서 수집했다.

MemAvailable 12 GiB 하한을 유지하기 위해 각 Java JVM의 heap 상한은 2 GiB, direct memory 상한은 1 GiB로 고정했다. .NET은 GC heap hard limit 2 GiB, Node는 old-space 2 GiB로 고정했다. 이 설정은 모든 해당 언어의 F/B와 무제한·paced 실행에 동일하게 적용했다. 이전 세션의 절대값과 직접 같다고 가정하지 않았다.

총 CPU는 `/proc/PID/stat`의 user와 system tick 차이를 완료 메시지 수로 나눴다. thread CPU는 `/proc/PID/task/TID/schedstat`의 runtime 차이다. schedstat와 process tick의 양자화·시점 차이는 residual 행에 남겼다. Core I/O send/receive는 thread의 gated sendto/recvfrom 호출 수 중 더 많은 방향으로 분류한 역할명이다. 혼합 방향 thread의 CPU를 syscall별로 다시 분해하지 않았다. CPU 역할의 원본 thread/TID, syscall 방향, 표본 수와 함수는 각 실행의 `profile-summary.json`에 있다. 소유 함수가 없는 일반 executor·ThreadPool 프레임은 other로 분류했다. C++은 perf CPU stack, Java는 perf와 JFR execution sample, .NET은 perf와 EventPipe CPU sampling, Node는 perf와 V8 CPU profile을 사용했다. Node native stack의 offline symbol 해석에서는 addr2line의 inline 확장을 끄고 V8 표본으로 JS 소유 함수를 보완했다.

네 언어의 pacing은 공유 submit ticket을 1 ms bucket으로 내려 같은 부하 형태를 적용했다. C++은 sleep_until, Java는 parkNanos, .NET은 nanosleep, Node는 timer를 사용했다. Node serial은 Atomics.wait를 사용한다. pacing은 payload의 측정 timestamp를 찍기 전에 수행했다.

thread wakeup은 별도 동일 rate 실행의 BPF sched_waking에서 깨우는 thread와 깨어나는 thread가 모두 측정 source/target에 속한 이벤트만 셌다. 동일 프로세스와 두 프로세스 사이의 wakeup을 나눠 표시했다. 이 행은 깨어남 시작을 센 handoff 대용값이다. sched_wakeup의 실행 문맥은 원래 waker가 아닐 수 있어 처음 수집한 해당 counter는 사용하지 않았다. 두 thread의 Event 왕복 1,000회 fixture에서 sched_waking이 각 방향 1,000회 이상을 잡는 것을 확인한 뒤 24개 조합을 다시 수집했다. 한 메시지의 논리적 queue 전달 횟수와 같지 않다. 이미 runnable인 thread로 전달하면 wakeup이 없을 수 있다. 별도 OS context switch 행은 각 thread의 자발적·비자발적 switch 합계다.

네 언어의 native allocation 행은 LD_PRELOAD로 malloc·calloc·realloc 호출 횟수와 요청 byte를 셌다. free는 byte allocation에 포함하지 않았다. aligned allocation 전용 API는 별도로 가로채지 않아 이 경로의 native allocation 전체를 보장하지 않는다. Java는 JFR의 ObjectAllocationInNewTLAB·ObjectAllocationOutsideTLAB도 기록했다. TLAB capacity bytes와 refill event는 allocation-summary.json에 보존했다. 표의 실제 객체 수는 JVMTI SampledObjectAlloc을 interval 0으로 켜 JVM heap object count와 size를 직접 셌다. interval 0 검증에서 byte[1024] 10,000개의 10,400,000 bytes와 count 10,000을 얻었다. Java native malloc은 이 숫자에 포함하지 않았다.

.NET bytes는 기존 bench의 GC.GetTotalAllocatedBytes(precise: true)를 사용했다. count는 EventPipe GCAllocationTick의 AllocationAmount64/ObjectSize 합계로 추정했다. 이는 모든 객체의 실제 count가 아니며, 표의 count를 추정으로 표시했다. 큰 객체와 작은 객체가 섞일 때 이 추정은 type/size 표본 편향의 영향을 받는다. Node는 V8 SamplingHeapProfiler interval 4,096 B와 major/minor GC로 수거된 객체 포함 옵션을 사용해 size/count를 추정했다. Node native malloc은 별도 LD_PRELOAD 값이다. V8 heap 표본은 JS heap을 대상으로 하며 Buffer의 외부 backing storage를 관리 heap bytes에 더하지 않았다. allocation 계측 실행의 CPU는 기본 CPU 표에 사용하지 않았다. 기본 CPU는 bench의 pacing·통계·빈 receive poll 비용도 포함한다. 특히 Node binding target은 zlink-raw-server/main.js:139의 setImmediate poll을 빈 구간에도 반복하므로 낮은 부하에서의 F−B CPU를 순수 제품 계층 비용으로 볼 수 없다. 이 차이는 allocation과 context switch에도 영향을 준다.

payload copy는 코드에서 1 KiB 본문이 새 저장소에 쓰이는 횟수다. protobuf encode와 소유 bytes decode는 copy에 포함하고, view·move와 metadata copy는 제외했다. 이는 동적 memcpy 호출 수가 아니며 allocator 내부 이동, Core와 kernel의 복사는 범위 밖이다.

TCP data packets는 tcpdump -Q in으로 중복을 제거한 loopback incoming capture의 TCP payload 길이가 양수인 packet 수다. HTTP stats 포트는 제외했다. 양쪽 socket TCP_INFO의 data_segs_out 차이도 함께 기록했다. loopback offload 때문에 capture packet과 TCP segment 수가 다를 수 있으므로 wire상의 물리 Ethernet packet 수로 해석하지 않았다.

처음 네 paced capture는 loopback에서 tcpdump -Q out이 packet을 수집하지 못해 무효 처리했다. -Q in으로 고친 뒤 같은 lock 세션에서 네 조합을 다시 측정했고, 표에는 재측정값만 사용했다. 원본과 무효 사유는 삭제하지 않았다. 초기 profile 세 개도 perf 기본 clock과 proc snapshot의 CLOCK_MONOTONIC이 달라 제외하고, --clockid mono를 명시해 같은 lock에서 재측정했다.

모든 유효 실행은 source errors·abandoned와 target errors가 0이고, request는 submitted=completed=target received를 확인했다. send는 completed=received+rejected를 확인했다. 검증기는 정상 fixture 3개와 의도적으로 count 또는 오류를 틀린 fixture 6개의 양쪽 결과를 시험했다. allocation counter에도 알려진 allocation 수와 bytes를 주입해 확인했다. native counter는 3 calls·15,360 bytes, Node 표본 검사는 100,000개 생성에서 104,930개 추정을 기록했다. TCP parser는 command 전용 port와 역방향 연결의 payload packet을 잡고 ACK-only packet을 제외하는 양쪽 fixture를 통과했다.

측정 세션은 2026-11-08T11:23:47 KST에 시작했다. 세션에서 기록한 최소 MemAvailable은 24.246 GiB다. SIGTERM/SIGINT만 사용했다.

세션 종료 시각은 2026-11-08T13:05:52 KST다. 모든 benchmark·collector를 종료한 뒤 lock을 해제했다.

빌드 구간에서 기록한 최소 MemAvailable은 16.364 GiB이며, 메모리 하한 때문에 종료한 빌드는 없었다.

원본 수치는 [summary.json](summary.json), 역할 분류는 [profile-summary.json](profile-summary.json), allocation event 해석은 [allocation-summary.json](allocation-summary.json)에 있다. 실행 명령·stats·pcap·trace는 runs/ 아래에 조합별로 보존했다. 종료한 benchmark의 JIT perf map은 symbols/로 옮겼으며, raw perf를 다시 symbol 해석할 때 해당 map을 /tmp의 원래 파일명으로 복원할 수 있다. 네 언어가 실제 로드한 Core library의 SHA-256은 모두 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01로 같았다. 실행 파일과 native library의 SHA-256은 [manifest.json](manifest.json)에 있다. 유효 168회는 오류·abandoned가 없고 count 검사를 통과했다. 무효 7회는 표에서 제외했고 미해결 실행 실패는 없다. 최종 검증은 [final-check.log](final-check.log)와 [check-cells.log](check-cells.log)에 있다.
