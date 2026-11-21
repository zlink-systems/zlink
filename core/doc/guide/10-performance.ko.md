---
title: "Core 성능"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Monitoring](06-monitoring.ko.md) | [다음: Core C API](02-core-api.ko.md)
<!-- zlink-nav:end -->

# Core 성능

> **이 장이 답하는 것** — 처리량과 지연에 영향을 주는 설정과 측정 방법을 정리한다.

Socket option을 바꾸기 전에 application 전체 경로를 측정한다. Message size, connection 수,
queue depth, transport와 TLS가 throughput과 latency에 영향을 준다.

## Backpressure와 HWM

Send와 receive HWM은 queue에 유지할 byte를 제한한다. Automatic HWM은 socket pattern, 설정한
profile, message 단위 byte와 connection 수를 사용해 제한된 queue 값을 선택한다. 적용된 계획은
`zlink_monitor_status()`로 확인한다.

Memory가 제한되면 COMPACT, 일반 workload에는 BALANCED를 사용한다. 추가 queue memory를
허용할 수 있을 때만 THROUGHPUT을 사용한다. Send-ready 알림은 재시도할 가치가 있다는 뜻이며 다음
send의 성공을 보장하지 않는다.

## Memory와 file descriptor

Idle connection 비용과 queued-message peak를 모두 계산한다. 예상 connection 수에 운영 여유를 더해
`RLIMIT_NOFILE`을 설정한다. `ZLINK_MAX_SOCKETS`는 transport connection이 아니라 socket handle
수를 제한한다.

## I/O thread 수와 배치

Context의 I/O thread 수 기본값은 4(`ZLINK_IO_THREADS_DFLT`)다. 각 연결은 연결을 만들 때 고른
I/O thread 하나에서 끝까지 처리되며, 연결은 I/O thread에 고르게 나뉜다.

작은 message의 요청-응답 위주 workload는 연결이 많아도 I/O thread마다 처리할 일이 적어
thread가 자주 잠들고 깨어난다. 이 경우 `zlink_ctx_set(ctx, ZLINK_IO_THREADS, n)`으로 I/O
thread 수를 줄이거나 socket option `ZLINK_OPT_AFFINITY`(I/O thread bitmask)로 연결이 쓰는
I/O thread 범위를 좁히면 빨라질 수 있다. Stream, 큰 message, 많은 data를 동시에 보내는
workload는 반대로 느려질 수 있으므로 바꾸기 전에 자기 workload로 측정한다.

참고 측정(Core multi bench, TCP, client 100 연결, client·server 모두 I/O thread 4 → 2,
3회 중앙값):

| Pattern | Message size | 처리량 변화 |
|---|---:|---:|
| DEALER_ROUTER request-reply | 64 B | +45% ~ +57% |
| DEALER_ROUTER request-reply | 1024 B | +23% ~ +31% |
| STREAM | 64 B · 1024 B | −25% ~ −29% |
| PUBSUB | 1024 B | −11% ~ −14% |
| DEALER_ROUTER send-send (C++ 다중 연결 서버) | 4 KB · 64 KB · 128 KB | −17% · −38% · −58% |

## Benchmark 절차

Core source를 바꾼 뒤 `core/build`를 다시 build한다. Benchmark runner는 실제 `libzlink` path를
출력하고 source보다 오래된 runtime을 거부해야 한다. Message size, connection 수, 실행 시간,
transport, TLS 설정, HWM profile, CPU 할당과 percentile 통계를 함께 기록한다.
