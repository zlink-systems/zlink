# ZLink Framework

이 디렉토리는 zlink `core`나 언어 `bindings` 자체가 아니라,
그 바인딩 위에 한 번 더 올라가는 `ZLink Framework` 작업 공간이다.

`ZLink Framework`는 성격상 framework adapter 계층으로 볼 수 있다.

이 계층의 목표는 아래와 같다.

- framework가 직접 통합할 축을 `ROUTER <-> ROUTER`, `SPOT`, `PUB/SUB`,
  `STREAM` 네 가지로 좁힌다.
- `ASP.NET Core`, `Spring`, `NestJS` 같은 기존 애플리케이션 프레임워크에
  zlink 기반 서버 간 메시징을 자연스럽게 붙인다.
- 프레임워크 사용자가 raw socket이나 low-level discovery 설정보다
  handler, client, event, DI 같은 익숙한 개념으로 작업하게 만든다.
- 기존 웹 서버 환경에서 흔히 두는 별도 gateway나 전용 로드밸런서 없이도
  `channel_name` 기준으로 직접 channel 호출을 가능하게 만든다.
- 현재 `core/`와 `bindings/`가 이미 제공하는 Discovery, Registry topology 조회,
  `SPOT` request/reply 같은 기반 기능을 프레임워크 친화적인 API로 다시 묶는다.
- `core` 계약과 `bindings` 계약을 직접 바꾸지 않고, 그 위에서 별도 라이브러리
  또는 패키지로 발전할 수 있는 구조를 잡는다.

문서 진입점:

- [ZLink Framework 문서](doc/README.ko.md) — 전체 진입점
- [공통 스펙](doc/framework/common/README.ko.md) — 언어 중립 정식 계약
- [언어별 문서](languages) — `.NET`·`C++`·`Java/Kotlin`은 정식, `Node.js`는 구현 기준, 그 외는 초안

`.NET`, `C++`, `Java/Kotlin`은 정식 문서로, `Node.js`는 구현 기준 문서로 승격되었고,
그 외 언어 문서(`Python`, `Go`, `Rust`)는 아직 초안 단계다.
