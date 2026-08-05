---
title: "Core 성능"
---

[English](10-performance.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Raw socket monitoring](06-monitoring.ko.md) | [다음: Core C API](02-core-api.ko.md)
<!-- zlink-nav:end -->

# Core 성능

> **이 장이 답하는 것** — 처리량과 지연에 영향을 주는 설정과 측정 방법을 정리한다.

Socket option을 바꾸기 전에 application 전체 경로를 측정한다. Message size, connection 수,
queue depth, transport, TLS와 callback 작업이 throughput과 latency에 영향을 준다.

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

## Benchmark 절차

Core source를 바꾼 뒤 `core/build`를 다시 build한다. Benchmark runner는 실제 `libzlink` path를
출력하고 source보다 오래된 runtime을 거부해야 한다. Message size, connection 수, 실행 시간,
transport, TLS 설정, HWM profile, CPU 할당과 percentile 통계를 함께 기록한다.
