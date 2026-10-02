---
title: "둘러보기 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/02-tour.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 둘러보기

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 1. 개요](01-overview.ko.md) | [다음: Kotlin Quickstart — 설치부터 첫 요청까지](../../quickstart.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/02-tour.ko.md) · [C#/.NET](../../../dotnet/guide/server/02-tour.ko.md) · [Java](../../../java/guide/server/02-tour.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/02-tour.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    ZLink의 channel 구성(ClientServer · Fanout · RouteMesh), Spot과 Actor, 무중단 이전,
    backpressure가 어떻게 움직이는지 직접 눌러 보며 확인할 수 있다.

MORPG 하나를 예로 들어 기능마다 단계를 하나씩 진행하는 브라우저 시뮬레이션이다. 위쪽 탭에서
기능을 고르고 단계마다 버튼을 누르면, 게임 화면과 서버 안의 메시지 흐름이 함께 움직인다. 실제
네트워크를 사용하지 않으며 시간과 수치는 시연용이다.

<iframe class="zlink-diagram" src="/common/diagrams/zlink-tour.html" title="ZLink 둘러보기 — MORPG로 보는 기능" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/zlink-tour.html" target="_blank">↗ 크게 보기</a></p>

| 탭 | 보여 주는 것 | 자세히 |
| --- | --- | --- |
| ClientServer | API 서버가 channel 이름으로 기능 서버를 부르고, weight에 따라 나눈다 | [Channel 메시징](20-channel-messaging.ko.md) |
| Fanout | 운영 툴이 한 번 발행하면 topic을 구독한 서버만 받는다 | [Channel 메시징](20-channel-messaging.ko.md) |
| RouteMesh channel | 던전 서버가 mesh 위에서 channel 이름으로 API 서버에 요청하고 weight로 나눈다. channel을 더해도 연결은 늘지 않는다 | [Channel 메시징](20-channel-messaging.ko.md#3-routemesh) · [Channel 동작 원리](30-channel-patterns.ko.md#42-후보-사이의-분배--기본값은-round-robin-값을-바꾸면-가중치-분배) |
| 로비와 던전 · RouteMesh | 접속부터 마을, 던전 입장, 보스전, 재접속, 귀환까지 | [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md) · [Session과 Actor 연결](24-actor-session.ko.md) |
| RouteMesh pub/sub | 경계 근처 player 위치를 topic으로 이웃 zone Spot에 전달하고, 경계를 벗어나면 그 사실도 전달한다(Logical Multicast) | [Channel 동작 원리](30-channel-patterns.ko.md#5-pubsub의-형태) · [ZoneWorld 따라 읽기](56-zoneworld.ko.md) |
| Instance Spot | 여러 서버가 같은 길드에 동시에 기여해도 한 줄로 처리된다 | [Spot](21-spot.ko.md) · [실행 모델](32-execution-model.ko.md) |
| Host Relocation | 전투 중에 서버를 점검해도 방이 다른 서버로 옮겨 가며 계속된다 | [Relocation](37-relocation.ko.md) |
| Backpressure | 같은 버스트를 받을 때 상한 없이 쌓는 서버와 ZLink 서버의 차이 | [Backpressure](33-backpressure.ko.md) |
