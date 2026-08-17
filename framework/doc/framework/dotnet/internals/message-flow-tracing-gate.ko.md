# Message flow tracing의 Off 무비용 게이트

이 문서는 .NET 런타임이 서버 spec 26(message flow tracing)의 Off 무비용 계약을
구현하는 방식을 기록한다. 계약 자체는 spec 26이 소유한다: Off에서는 live level을
읽고 분기하는 것 외의 trace 전용 작업(이벤트 객체·attribute·문자열·timestamp·
sampling hash·telemetry queue item 생성, provider 호출)을 하지 않는다.

## 구현 규칙

- **게이트가 이벤트 생성보다 먼저다.** `ZLinkMessageFlowTracer.Enabled(outcome)`
  는 live level의 저렴한 읽기이며, 호출부는 이 게이트를 통과한 뒤에만 이벤트를
  만든다.
- **드문 경로는 `TraceLazy(outcome, Func<ZLinkMessageFlowEvent>)`를 쓴다.**
  delegate는 게이트 통과 후에만 실행되므로 Off에서 이벤트가 생성되지 않는다.
  단, closure 할당 자체가 부담인 message-hot path는 `if (Enabled(outcome))`
  가드를 유지해 closure 할당도 피한다.
- **`TraceDispatchError`는 Off면 즉시 반환한다.** dispatch error의 flow 상관
  필드 보정과 log/telemetry 전달은 게이트 뒤에서만 일어난다.
- **provider 호출 실패는 message operation 결과를 바꾸지 않는다.** 예외는
  콜백 실패 보고 경로로만 흡수한다.

## 검증

`MessageFlowTracerTests`가 Off 침묵, 런타임 level 전환(off↔normal) 후 다음 처리
지점부터의 적용, error/drop/backpressured 비샘플링을 고정한다. 다른 언어와의
어휘 패리티(event id 2종, phase 9종, 폐쇄 소문자 어휘)는 공통 spec 26 §6의
contract test 요구를 따른다.
