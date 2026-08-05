---
title: "Raw socket monitoring"
---

[English](06-monitoring.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: TLS와 WSS](05-tls-security.ko.md) | [다음: Core 성능](10-performance.ko.md)
<!-- zlink-nav:end -->

# Raw socket monitoring

> **이 장의 계약 소유 문서** — [Monitoring](../spec/core/07-monitoring.ko.md)이 다룬다.
> 이 챕터는 연결 상태를 관측하는 방법을 사용법 중심으로 설명한다.

Socket monitor는 data receive 경로를 바꾸지 않고 transport와 protocol event를 제공한다.
`zlink_socket_monitor_open()`으로 monitor를 열고 한 가지 소비 방식을 선택한다.

## Receive 방식

`zlink_socket_monitor_recv()`를 직접 호출하거나 monitor를 poller에 등록한다. 기존 event loop가
scheduling을 담당할 때 사용한다.

## Callback 방식

`zlink_socket_monitor_handler()`를 등록한다. Callback은 관찰 대상 socket의 I/O thread가 아니라 Core
control runtime에서 실행된다. 다음 monitor와 timer callback이 지연되지 않도록 callback을 짧게 유지한다.

## Snapshot

`zlink_monitor_status()`는 pending message와 automatic HWM field를 포함한 현재 raw socket state와
진단 counter를 반환한다. Snapshot은 한 시점의 관찰 결과다. 전이 순서가 필요하면 event stream을 사용한다.

## 종료

`zlink_monitor_close()`로 monitor를 닫는다. Socket을 닫아도 monitor source가 종료된다. 두 handle을
함께 polling하는 코드는 어느 경로에서든 termination이 반환될 수 있음을 처리해야 한다.

Monitor event에는 application payload나 topology state가 포함되지 않는다.
