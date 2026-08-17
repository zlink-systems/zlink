# 이벤트 루프 인터록과 relocation 순서 규칙

이 문서는 Node 런타임이 actor relocation·세션 relay·in-flight 요청을 구현할 때
이벤트 루프(단일 스레드) 위에서 지키는 순서 불변식을 기록한다. 관찰 가능한
계약은 공통 서버 spec이 소유한다. Node에는 병렬 경합이 없지만, `await` 경계가
곧 인터리빙 지점이므로 "어떤 상태 전이가 어느 await 앞에 있어야 하는가"가
언어 고유의 구현 세부가 된다.

## 불변식

- **activeFrames는 route 교체를 넘어 공유되는 카운터다.** route 객체에 매달면
  교체 시 계수가 소실되어 drain이 조기 완료된다.
- **command 42(routed submit)는 딜라인 후 SubmitResult를 검사한다.** 성공 큐잉과
  전달 성공을 구분하지 않으면 상대 노드 부재가 조용히 삼켜진다.
- **one-way relay는 accept-early, terminal은 detached로 처리한다.** relay 수신을
  caller 완료 조건에 묶으면 mailbox 경계(spec 05)가 깨진다.
- **in-flight REQUEST는 캡처-완료 후 응답을 detached로 보낸다.** relocation 중
  이던 요청의 응답 경로가 새 route에 붙기 전에 await로 양보하면 응답이 옛
  route로 나간다.
- **deferred-join journal은 relocation envelope를 만들기 전에 CAS 경로를 보존
  한다.** envelope 생성 후에 CAS를 하면 command 34 post-commit이 실패한다.
- **자기 노드 대상 command 42/44는 로컬 디스패치한다.** RouteMesh는 자기 자신
  과의 연결이 없으므로 원격 경로로 보내면 NotConnected가 된다. 이때 소스 세대
  해석도 로컬(`sourceLifecycleGeneration()`, bigint 반환)로 한다. 자기-이전
  (fence에 session-owner 정체성이 들어가는 경우)은 selfSeal/selfSealed/selfRoute
  픽스처로 계약 테스트에 고정되어 있다.

## 배경

이 규칙들은 m6a/m6b/m6c 게이트와 6개 샘플 실기에서 확립됐다. 증상은 대부분
"relocation 직후 요청이 NotConnected 또는 응답 유실"로 나타난다.
