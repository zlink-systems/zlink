# Sample/E2E 설정 정책

이 문서는 모든 framework 언어의 sample과 E2E에서 애플리케이션 설정을 읽고 전달하는 공통
규칙을 정의한다. Sample과 E2E는 사용자가 참고하는 실행 예시이므로, shell이나 PowerShell이
애플리케이션의 설정 시스템을 대신하면 안 된다.

이 정책은 공개 framework API 계약을 새로 정의하지 않는다. 각 언어의 framework host가 채택해야
하는 정식 설정 시스템으로 값을 읽고, 검증된 설정을 framework builder에 적용하는 방법을 고정한다.
아래 언어별 기준은 현재 구현이 모두 완료되었다는 설명이 아니라 sample과 E2E가 도달해야 하는
적용 목표다.

## 1. 적용 범위

다음 값은 모두 이 정책의 애플리케이션 설정에 해당한다.

- server와 client가 사용하는 endpoint
- Redis endpoint와 key prefix
- instance 이름과 routing id
- 애플리케이션 요청의 timeout과 retry 한도
- 로그, trace, evidence와 업무 상태 파일 경로
- TLS 인증서와 key 파일 경로
- codec, monitoring과 framework 기능 옵션

Runner는 실행별 port, Redis endpoint와 임시 디렉터리를 결정할 수 있다. 그러나 결정한 값을
환경 변수로 애플리케이션에 전달하면 안 된다. Framework host에는 role별 설정 파일 경로를
전달한다. Framework host가 아닌 standalone client는 직접 연결하는 endpoint, 요청 timeout과
scenario selector를 명시적인 CLI option으로 받을 수 있다.

## 2. 필수 규칙

### 2.1 Framework host는 설정 파일을 사용한다

- 각 server role은 자기 역할에 필요한 값만 포함한 설정 파일 하나를 받는다.
- 기본 설정 파일은 언어별 관례에 맞는 `Configuration/`, `config/` 또는 resource 디렉터리에 둔다.
- 동적 port와 실행별 Redis가 필요하면 runner가 임시 role별 설정 파일을 생성한다.
- Framework host 실행 파일의 CLI에는 `--config <path>` 또는 해당 언어 host가 제공하는 동일한
  의미의 설정 파일 경로만 전달한다.
- Server role마다 별도 실행 진입점을 사용한다. `--role`이나 `--mode`로 하나의 실행 파일을 여러
  server role로 전환하지 않는다.
- Endpoint, timeout, routing id와 E2E scenario selector를 framework host의 개별 CLI option으로
  전달하지 않는다.
- 설정 파일이 없거나 필수 값이 잘못되면 프로세스는 framework runtime을 시작하기 전에 실패한다.

### 2.2 환경 변수로 애플리케이션 설정을 전달하지 않는다

**Sample과 E2E의 애플리케이션 코드에서 직접 사용할 수 있는 환경 변수는 0개다.** Server,
client, handler와 애플리케이션의 configuration 경계는 용도와 이름에 관계없이 환경 변수를 직접
읽지 않는다.

Sample과 E2E의 runner, server, client는 애플리케이션 설정에 환경 변수를 사용하지 않는다.
Runner가 환경 변수를 설정한 뒤 하위 프로세스가 읽는 방식도 같은 금지 대상이다.

다음 직접 접근은 sample과 E2E 애플리케이션 코드에서 사용하지 않는다.

| 언어 | 금지하는 직접 접근 |
|------|--------------------|
| Node.js | `process.env` |
| .NET | `Environment.GetEnvironmentVariable(...)` |
| Java/Kotlin | `System.getenv(...)`, `System.getProperty(...)` |
| C++ | `std::getenv(...)`, `getenv(...)`, sample/E2E 설정을 위한 `load_env(...)` |

언어별 설정 시스템이 환경 변수를 기본 provider로 포함하더라도 sample과 E2E 설정에는 적용하지
않는다. 설정 파일에 있는 값보다 환경 변수가 우선하도록 구성하거나, runner가 그 provider를 통해
값을 주입하면 안 된다.

### 2.3 검증된 typed 설정만 사용한다

- 설정 파일 parsing, 기본값 적용과 필수 값 검증은 `Configuration/` 경계 한 곳에서 수행한다.
- Server module, handler, store와 client scenario가 설정 파일을 각각 다시 읽지 않는다.
- 설정을 사용하는 구성 요소는 언어별 DI 또는 typed binding으로 완성된 설정 객체를 받는다.
- Framework builder는 검증된 설정 객체를 입력으로 사용한다. Builder 내부나 module factory에서
  전역 환경을 다시 조회하지 않는다.
- 로그 경로, evidence 경로와 role 이름도 topology endpoint와 같은 설정 객체에 포함한다.

## 3. 언어별 적용 목표

| 언어 | 목표 설정 입력과 binding | Framework 연결 |
|------|---------------------|----------------|
| Node.js/NestJS | 설정 파일을 `@nestjs/config`의 `ConfigModule` typed provider로 읽고 시작 시 검증한다. | Typed provider를 `ZLinkModule.forRootFactory(...)`에 주입한다. |
| .NET/ASP.NET Core | 설정 파일을 `IConfiguration`으로 읽고 Options 타입에 binding한 뒤 검증한다. | 검증된 Options를 `AddZLinkFramework(...)` 구성에 사용한다. |
| Java/Kotlin/Spring Boot | `application.yml` 또는 `application.properties`를 `@ConfigurationProperties` 타입에 binding하고 검증한다. | `ZLinkFrameworkConfigurer`가 binding된 설정을 builder에 적용한다. |
| C++ framework host | JSON을 `app.config().load_json(...)`으로 읽고 `bind<T>()` 또는 `bind_required<T>()`로 변환한다. | Binding 결과를 framework host 구성에 사용한다. |

한 언어에만 별도 설정 전달 helper나 병렬 설정 추상화를 만들지 않는다. 해당 언어 host의 정식 설정
시스템으로 표현할 수 없는 요구가 있으면 sample이나 E2E에서 우회하지 않고 별도 설계 gap으로
기록한다.

## 4. Framework host가 아닌 client

Stream Connector client, HTTP client와 browser client는 framework host가 아닐 수 있다. 이 경우
server용 framework module이나 별도 configuration 시스템을 만들지 않는다.

- Standalone client는 client가 직접 연결하는 endpoint, 요청 timeout과 E2E scenario selector를
  명시적인 CLI option으로 받을 수 있다. 시작할 때 필수 값, 형식과 범위를 한 번 검증하고 typed
  client 설정 객체로 변환한다.
- Redis, routing id, server role, 전체 topology, 파일 경로, credential 또는 framework option이
  필요하거나 list와 중첩 구조를 전달해야 하면 client 설정 파일을 사용한다. Secret처럼 CLI에
  노출하면 안 되는 값도 설정 파일로 전달한다.
  이 경우 파일 읽기와 검증은 client 진입점의 설정 경계 한 곳에서만 수행한다.
- Browser client는 정적 `config.json` 또는 runner가 제공하는 `/config.json`을 읽는다.
- Client scenario에는 endpoint 상수, 환경 변수 조회와 server topology 전체를 넣지 않는다.
- E2E scenario selector는 실행 제어 입력이므로 CLI로 받을 수 있다.

## 5. Runner와 도구 환경의 경계

이 정책은 sample/E2E 애플리케이션 설정을 대상으로 한다. 운영체제와 도구가 프로세스를 실행하기
위해 사용하는 기존 환경까지 애플리케이션 설정으로 간주하지 않는다. 예를 들어 실행 파일 탐색,
언어 runtime 위치와 native library loader가 요구하는 표준 환경은 runner가 그대로 상속할 수 있다.
이는 운영체제와 도구 프로세스를 실행하기 위한 허용 범위일 뿐이다. Sample과 E2E 애플리케이션
코드가 직접 읽을 수 있는 환경 변수는 여전히 0개다.

다만 sample/E2E가 소유하는 새 환경 변수 interface를 만들어 설정 우회 경로로 사용하면 안 된다.
Docker image, build directory처럼 runner 자체의 선택값이 필요하면 runner option이나 repository의
runner 설정으로 관리한다. Readiness 대기 한도도 애플리케이션 timeout과 구분해 runner 설정으로
관리한다. 애플리케이션 프로세스는 이 값을 읽지 않는다.

## 6. Secret과 임시 설정 파일

- 인증서 private key나 credential을 저장소의 기본 설정 파일에 기록하지 않는다.
- Secret이 필요한 E2E는 runner가 실행별 임시 secret 파일을 만들거나 외부 secret file 경로를
  설정 파일에 기록한다.
- Runner가 만든 설정과 secret 파일은 현재 사용자만 읽고 쓸 수 있게 권한을 제한한다. POSIX
  환경에서는 파일 mode를 `0600`으로 설정한다. Windows에서는 상속 ACL을 제거하고 runner를 실행한
  사용자에게만 읽기와 쓰기 권한을 부여한다. 권한을 제한하지 못하면 프로세스를 시작하지 않는다.
- 정상 종료와 실패 종료 모두에서 runner가 자신이 만든 임시 파일을 정리한다.
- 실패 분석을 위해 설정을 보존해야 하면 secret 값을 제거한 복사본만 남기고 보존 경로를 출력한다.
  Secret이 들어 있는 원본 임시 파일은 삭제한다.

## 7. Runner 실행 순서

개별 sample/E2E runner는 다음 순서를 따른다.

1. 실행별 디렉터리와 port를 준비한다.
2. 필요한 Redis container를 만들고 실제 endpoint를 확인한다.
3. 각 role의 설정 파일을 생성한다.
4. Role 실행 파일에 설정 파일 경로를 전달한다.
5. Readiness를 확인한 뒤 standalone client에 필요한 CLI option을 전달해 self-check 또는 E2E
   scenario를 실행한다.
6. 자신이 시작한 프로세스, Redis와 임시 설정 파일을 정리한다.

Runner의 설정 관련 책임은 설정 파일 생성과 경로 전달로 제한한다. 설정 파일 parsing, 필수 값
검증, server별 framework builder 구성, 설정 기본값과 설정 항목의 의미를 shell, PowerShell 또는
공통 runner에 다시 구현하지 않는다.

### 7.1 Runner 단순성 규칙

Sample과 E2E의 runner는 사용자가 실행 순서를 바로 이해할 수 있게 유지한다. 단순함은 파일의 줄
수가 아니라 runner가 맡는 책임, 조건 분기, 중복 구현과 우회 경로의 수를 기준으로 판단한다.

- Shell과 PowerShell script는 인자를 확인하고 공통 runner를 호출하는 간단한 진입점으로 둔다.
- Runner는 실행 준비, role 설정 파일 생성, server 순차 실행, readiness 확인, client 실행, 결과
  확인과 정리만 담당한다.
- Framework builder 구성, 설정 기본값과 설정 항목의 의미를 runner에 다시 구현하지 않는다.
- 환경 변수 호환 경로, 여러 단계의 fallback과 기존 프로세스 자동 탐색을 추가하지 않는다.
- 오류가 발생하면 즉시 실패하고, 해당 실행에서 runner가 만든 프로세스, container와 임시 파일만
  정리한다.
- 같은 실행 로직을 shell, PowerShell과 언어별 script에 각각 중복해서 작성하지 않는다.
- 여러 sample이나 E2E가 실제로 공유하는 실행 동작만 공통 runner로 분리한다.
- 모든 sample의 차이를 조건 분기로 처리하는 하나의 범용 runner를 만들지 않는다. Sample별 차이는
  role 목록, 실행 명령과 설정 파일 같은 명시적인 입력으로 표현한다.
- 공통 runner에는 프로세스 시작, readiness 대기와 정리처럼 같은 의미로 재사용되는 동작만 둔다.
  Sample 이름에 따른 `if`나 `switch`가 필요하면 해당 실행 순서를 sample별 runner로 분리한다.
- Redis container와 여러 server role처럼 scenario에 필요한 절차는 유지한다. 실행에 필요하지 않은
  자동 탐색, 호환 처리와 중복 fallback을 단순화 대상으로 본다.

기본 실행 흐름은 준비, role 설정 파일 생성, server 순차 실행, readiness 확인, client scenario
실행, 결과 확인, 정리 순서로 유지한다. Scenario가 요구하지 않는 별도 단계는 추가하지 않는다.

## 8. 회귀 검사

각 언어의 sample/E2E 회귀 검사는 다음 조건을 확인한다.

- Runner가 endpoint, Redis, role과 로그 경로를 환경 변수나 JVM system property로 전달하지 않는다.
- 애플리케이션 코드가 환경 변수를 직접 읽지 않는다.
- 언어별 설정 시스템에 환경 변수와 JVM system property provider를 등록하지 않거나 기본 provider
  목록에서 명시적으로 제거한다. 설정 파일의 key와 같은 이름의 외부 값이 존재해도 binding 결과가
  바뀌지 않는지 확인한다.
- Framework host가 설정 파일 경로를 받고 언어별 설정 시스템으로 binding한다.
- Framework host가 설정 파일 경로 이외의 CLI option으로 설정을 받거나 덮어쓰지 않는다.
- Standalone client가 CLI 입력을 시작할 때 한 번 검증하고 typed 설정 객체로 변환한다.
- 필수 설정 누락과 잘못된 endpoint가 runtime 시작 전에 실패한다.
- Runner가 만든 설정 파일에는 실행별 port와 Redis endpoint가 실제로 반영된다.
- Browser client가 환경 변수 없이 `config.json`을 읽는다.

기존 sample이나 E2E가 이 정책과 다르면 현재 동작을 예외로 인정하지 않는다. 해당 항목은 migration
gap으로 기록한다. Framework host는 설정 파일과 typed binding으로 전환하고, standalone client는
검증된 CLI 입력 또는 필요한 경우 typed 설정 파일로 전환한 뒤 완료로 판정한다.
