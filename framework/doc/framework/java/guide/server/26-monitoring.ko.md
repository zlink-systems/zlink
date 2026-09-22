---
title: "모니터링 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/26-monitoring.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 모니터링

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Location](25-location.ko.md) | [다음: 실행 모델](32-execution-model.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/26-monitoring.ko.md) · [C#/.NET](../../../dotnet/guide/server/26-monitoring.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/26-monitoring.ko.md) · [Node/TypeScript](../../../node/guide/server/26-monitoring.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    지금 무엇이 준비되었는지 읽고, 그 상태가 바뀔 때마다 받고, message 하나가 어디서
    끝났는지 남길 수 있다.
    이 장의 코드는 [예제 저장소의 tutorial README](https://github.com/zlink-systems/zlink-java-examples/blob/main/tutorial/README.ko.md)에서 가져온 언어별 관측 표면의 최소 호출이며, README의 「내려받기와 설치」·「빌드」·「실행」 절을 따르면 process의 상태와 기록을 읽을 수 있다.

앞 장들은 등록하고 호출하는 쪽을 다뤘다. 돌기 시작하면 다른 것이 필요해진다 — 연결이
준비되었는지, 어느 상대가 빠졌는지, message가 어디서 실패했는지다. handler를 아무리 읽어도
그 답은 나오지 않는다. **Framework는 그것들을 공개 표면으로 제공한다.**

runtime의 사건을 handler로 받는 표면은 없다. 운영 endpoint에는 먼저 상태 조회를 사용하고, 상태
변화를 처리해야 할 때만 구독을 추가한다. message 하나의 처리 경로는 진단 기록으로 보고, 대시보드
수치는 [운영과 lifecycle](12-operations.ko.md#2-런타임-메트릭)의 계기로 수집한다.

## 1. 운영 상태 읽기로 시작하기

<iframe class="zlink-diagram" src="/common/diagrams/26-observation-paths.html" title="관측 표면의 종류" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/26-observation-paths.html" target="_blank">↗ 크게 보기</a></p>

이 흐름은 운영 endpoint가 현재 상태를 즉시 응답하면서도, 필요한 곳에서만 변화와 message 처리 경로를
따로 관찰하게 한다.

## 2. 지금 상태 읽기

조회는 **호출 시점의 값 한 장**을 돌려준다. 운영 endpoint의 응답을 만들거나 한 번만
확인할 때 사용한다.

```java
--8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/NodeLivenessObserver.java:doc-zw-observe-peers"
```

**준비 여부와 상태 값을 함께 읽는다.** 준비되지 않았다는 것만으로는 무엇을 할지 정할 수
없고, 준비되지 않은 이유가 따로 온다 — 받을 수 있는 상대가 없는 것과 기록 저장소가 멎은
것과 지금 비우는 중인 것은 대응이 다르다.

돌아오는 값에는 상대의 식별자와 지금 상태와 사용할 수 없는 이유만 담긴다. 재연결 시도 횟수나
socket 내부 상태는 공개 계약이 아니다.

## 3. 변화 구독하기

구독은 **바뀔 때마다 그 뒤의 완전한 값**을 준다. 바뀐 항목만 담은 사건이 아니라 매번
전체다. 이전 값과 비교할 일이 있으면 구독하는 쪽이 보관한다.

```java
--8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/NodeLivenessObserver.java:doc-zw-observe-peers"
```

**놓칠 수 있다.** 받는 쪽이 느리면 보관 한도를 넘는 중간 값을 건너뛴다. 놓친 개수는 항목마다
함께 오고, 합치기로 건너뛴 것과 한도를 넘겨 다시 받을 수 없는 것으로 나뉜다. 앞의 것은 최신
값을 받았다는 뜻이지만 뒤의 것은 그렇지 않다.

두 값의 선후는 항목에 실린 순번으로 판단한다. 함께 오는 시각은 표시용이다.

조회는 한 번 읽을 때, 구독은 상태 전이를 기록하거나 반응할 때 사용한다. 중간 값을 건너뛰는 것은
구독뿐이다.

구독은 취소할 때까지 열려 있으므로 **host의 수명에 묶인 자리에서 돌린다.** 언어마다 그
자리가 다르다 — 백그라운드 서비스, 구독 객체의 수명, 취소 signal이다.

## 4. 진단 수준 정하기

message 하나가 어디서 어떻게 끝났는지는 진단이 남긴다. 수준은 다음과 같다.

```java
--8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java:doc-monitoring-flow"
```

| 수준 | 남기는 것 |
| --- | --- |
| 끔 | 남기지 않는다. trace 항목도 문자열도 만들지 않는다 |
| 오류(기본) | dispatch 실패와 backpressure |
| 보통 | 위에 더해 수신·dispatch·완료 같은 주요 전이 |
| 상세 | 위에 더해 byte 크기와 걸린 시간 |

**운영에서는 오류 단계로 두고 필요할 때만 올린다.** 상세 단계는 message마다 기록을 남기므로
처리량이 많은 구간에서는 그 자체가 부하가 된다. 출력만 버리는 기록 filter는 끔 단계와 같지
않다 — 만드는 비용이 그대로 남는다.

한 요청과 그 답을 잇는 값과, 그 요청이 시작한 뒤따르는 호출까지 잇는 값이 각각 기록에
실린다. 여러 node를 거친 조각을 하나로 묶어 볼 때 사용한다.

기록을 어디에 저장하고 어디로 내보낼지는 **application의 기존 설정이 소유한다.** Framework는
관측용 callback이나 파일 경로를 공개하지 않고, application이 구성해 둔 표준 provider에 사용한다.
provider 호출이 실패해도 원래 메시지 처리의 결과는 바뀌지 않는다.

## 5. 자주 발생하는 문제

- **runtime의 사건을 handler로 받고 싶다** — 그런 표면은 없다. 상태 변화는
  [변화 구독하기](#3-변화-구독하기)로, 개별 message의 결과는
  [진단 수준 정하기](#4-진단-수준-정하기)로 본다.
- **구독이 아무것도 주지 않는다** — 그 이름에 변화가 없으면 조용하다. 지금 값이 필요하면
  [지금 상태 읽기](#2-지금-상태-읽기)를 먼저 읽고 구독을 이어 받는다.
- **health endpoint를 기대한다** — Framework는 HTTP endpoint를 만들지 않는다. 준비 여부를
  application의 기존 endpoint에 연결한다 —
  [운영과 lifecycle](12-operations.ko.md#5-운영-호출과-readiness-연결)이 그 자리다.
- **어느 node에 무엇이 있는지 보고 싶다** —
  [Location](25-location.ko.md)의 조회와
  [운영과 lifecycle](12-operations.ko.md#6-location-readiness와-운영-조회)의 topology 조회를 사용한다.
- **handler가 없는 message를 알고 싶다** — 진단 수준을 오류 이상으로 두면 dispatch 실패로
  남는다. 요청은 오류 응답으로 돌아오고 보내기는 조용히 버려지므로, 보내기 쪽은 진단으로만
  확인된다 — [Channel 동작 원리](30-channel-patterns.ko.md#7-호출이-끝났다는-것의-의미)가 그
  차이를 다룬다.

## 6. 확인

Server와 Client를 실행한 뒤 상태 조회에서 준비 상태와 peer 상태를 확인하고, 진단 수준을 높여 보낸
message의 수신·dispatch·완료 기록을 확인한다.

## 7. 관련 문서

- 수치와 운영 호출 — [운영과 lifecycle](12-operations.ko.md)
- 준비되지 않은 이유의 종류 — [Channel 동작 원리](30-channel-patterns.ko.md#6-연결과-discovery)
- 처리보다 도착이 빠를 때 — [Backpressure](33-backpressure.ko.md)
- 언어별 표면 이름 — [주요 타입 사용 색인](13-interface-catalog.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
