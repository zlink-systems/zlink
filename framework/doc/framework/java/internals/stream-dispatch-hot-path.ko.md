# Stream 수신 dispatch hot path

이 문서는 Java Stream Connector가 수신 message를 handler에 전달할 때의
수명과 allocation 기준을 기록한다. public API 계약은 공통 Stream
Connector spec과 Java Stream Connector interface spec이 소유한다.

## Handler 목록의 동시성

수신 handler 목록은 packet 이름별로 `CopyOnWriteArrayList`에 저장한다. 등록과 제거가
수신 callback과 동시에 일어날 수 있으므로 callback은 해당 목록의 iterator가 제공하는
snapshot을 사용한다. 수신 message마다 `List.copyOf`로 snapshot을 다시 만들지 않는다.
이 방식은 registration 변경의 동시성 의미를 바꾸지 않으면서 message당 임시 list
allocation을 제거한다.

handler가 없는 message도 handler dispatch가 실행되지 않으면 queue에 보관될 수 있으며,
wait 호출이 그 message를 소비할 수 있다. queue가 message를 dispatch 대상으로 선택한
뒤 handler가 사라진 경우에는 queue가 소유한 encoded payload를 닫는다. handler가 있으면
기존과 같은 순서로 각 callback에 독립된 payload object를 제공하고, queue가 소유한
payload의 종료 시점도 유지한다. 이 최적화는 payload object의 ownership이나 callback
수명을 공유하도록 바꾸지 않는다.

## 검증

이 구조는 dispatch, queue 보관과 payload 종료의 기존 경계를 유지한다.
public contract나 binding API를 추가하지 않으며, message별 codec과 raw-frame 우회도
사용하지 않는다.
