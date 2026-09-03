---
title: "Core 용어"
---

[English](https://zlink-systems.github.io/zlink/spec/core/glossary/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md)
<!-- zlink-nav:end -->

# Core 용어

> **이 장이 답하는 것** — Core 스펙 전반에서 공유하는 핵심 용어의 짧은 정의. 각 용어의
> 정확한 계약은 해당 spec 문서가 소유하며, 이 문서는 다른 문서가 첫 사용 시 링크하는 대상이다.

### Context

I/O thread와 socket을 담는 zlink의 최상위 container다. 정확한 계약은 [Context](01-context.ko.md)가 소유한다.

### I/O thread

Context가 만들고 관리하는 background thread다. 네트워크 송수신을 실제로 처리한다.

### socket

message를 주고받는 endpoint다. 반드시 Context에 속한다. 계약은 [Socket 공통](socket/README.ko.md)이 소유한다.

### HWM

High-Water Mark. queue에 유지할 byte를 제한해 backpressure를 적용하는 값이다.

### backpressure

downstream이 처리 속도를 따라오지 못할 때 sender의 추가 제출을 제한하는 동작이다.

### Auto HWM budget

Core가 memory 입력에서 계산해, application queue들의 HWM을 나눌 때 기준으로 삼는 byte 총량이다. 계약은 [Auto HWM](systems/06-auto-hwm.ko.md)이 소유한다.

### directional queue

한 application 방향의 message를 담는 물리 queue다. 두 endpoint가 같은 방향을 관찰해도 한 번만 집계한다.

### generation

같은 방향 queue나 transport pair를 다시 만들 때 이전 것과 구분하는 버전 번호다. Reconnect마다
증가하며, 이전 generation의 REPLY·FLOWSTATE는 새 generation에서 폐기한다.

### effective cap

Auto HWM budget에 씌우는 상한이다. profile 고정 cap과 활성 queue 하한 합계 중 큰 쪽이다.

### water-filling

남은 budget을 아직 상한에 못 미친 queue들에 물을 붓듯 고르게 채워 나누는 분배 방식이다.

### completion progress lane

ROUTER-ROUTER가 양방향 request wait cycle을 끊기 위해 사용하는 별도 physical connection이다.
Terminal reply·error reply와 receive-flow control을 운반하며 HWM admission과 Application budget
계산에서 빠진다. DEALER-ROUTER에는 이 lane이 없고 reply가 single Application connection을
사용한다. `zlink_completion_recv()`가 소비하는 socket-local completion queue는 이 physical
connection과 다른 개념이다.
