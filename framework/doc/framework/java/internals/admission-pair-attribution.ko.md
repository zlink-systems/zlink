# Admission의 transport pair 귀속 규칙

이 문서는 Java 런타임이 mesh admission(HELLO/ADMIT/REJECT/UPDATE)과 liveness를
구현할 때 스레드 경합 아래에서 지키는 pair 귀속 불변식을 기록한다. 관찰 가능한
계약은 공통 서버 spec이 소유한다. Java는 진짜 병렬 스레드에서 monitor 이벤트와
admission 응답이 도착하므로, 모든 이벤트를 "어느 물리 연결(pair)의 것인가"로
귀속하는 규칙이 없으면 좀비 연결이 산다.

## 불변식

- **monitor DISCONNECT는 endpoint FIFO가 아니라 exact pair에 귀속한다.**
  같은 endpoint로 여러 연결이 생길 수 있으므로 "가장 오래된 연결이 끊겼겠지"
  식의 FIFO 귀속은 산 연결을 죽이고 죽은 연결을 살린다.
- **RID liveness ACK는 pair에 귀속한다.** RID만 보고 liveness를 갱신하면 같은
  RID를 가진 좀비(끊긴 옛 연결)가 계속 산 것으로 갱신된다.
- **pair-id는 transport lane마다 다르다.** 서로 다른 lane에서 온 이벤트를
  selected-pair와 동등 비교로 매칭하지 않는다. reverse-ownership 필터와 probe
  RID 주소로 판정한다.
- **같은 pair에서는 HELLO/ADMIT direction을 무시하고 connection-id를 재사용
  한다.** 방향별로 새 connection-id를 만들면 한 물리 연결이 두 개의 논리 연결로
  보인다.
- **dead-current 연결의 evict에는 최소 연령 가드(6초)를 둔다.** 막 수립된
  연결이 monitor 이벤트 지연 때문에 dead로 보이는 순간 evict하면 admission이
  수렴하지 않는다.
- **pair-scoped REJECT 전송이 실패해도 RID 주소로 재전송하지 않는다.** REJECT를
  RID로 재전송하면 다른(산) pair에 도착할 수 있다. RID 폴백은 ADMIT/UPDATE에만
  허용한다.
- **비동기 REJECT는 fence 검증과 경합하지 않게 처리한다.** REJECT 처리 경로가
  owner fence 검증과 병행하면 단독 실행에서는 통과하는 테스트가 전체 실행에서
  실패한다.

## 배경

이 규칙들은 M6A 게이트(953 tests)와 admission 수렴 실기에서 확립됐다. 하나라도
어기면 증상은 대부분 "샘플 실기에서 노드 간 요청이 간헐적으로 응답 없음"으로
나타나며, 단위 게이트에서는 결정적 재현이 어렵다.
