# Framework option builder naming

이 문서는 framework option builder 이름을 정할 때 적용하는 공통 원칙이다.
언어마다 표기법은 다를 수 있지만, 이름이 드러내는 의미는 같아야 한다.

## 기본 원칙

framework option builder 는 설정 객체를 조회하는 표면이 아니라, runtime 등록 모델을
만드는 표면이다. 따라서 상태를 추가하거나 역할을 켜는 메서드는 그 동작이
이름에 드러나야 한다.

- `add*` 는 새 등록 항목이나 반복 가능한 값을 추가할 때 사용한다.
  예: `addClientServerChannel`, `addRegistryEndpoint`, `add_forwarded_metadata_key`
- `enable*` 는 이미 선택한 등록 항목 안에서 역할을 켤 때 사용한다.
  예: `enableServer`, `enableClient`, `enable_publisher`
- `use*` 는 기존 등록 항목에 적용되는 선택 정책이나 설정 영역을 열 때 쓴다. 그 안에서
  실제로 값을 추가하는 메서드는 `add*` 로 명확하게 쓴다. 정책을 여는 것뿐 아니라 **정책 enum 하나를
  기존 등록에 직접 선택**하는 경우도 `use*` 가 맞다(값 하나에 하위 option 객체를 강요하면 얕은 층이
  생기므로 `configure*` 보다 낫다).
  예: `useDiscovery`, `useRegistrySpotRemoteAddresses`, `use_handler_group`,
  `useDrainPolicy(ZLinkSpotDrainPolicy.ReleaseAndRecreate)`
- `configure*` 는 하위 option 객체를 넘겨 세부 값을 바꿀 때 사용한다.
  예: `configureDispatch`, `configure_metadata`
- `bind` 는 local endpoint 를 열 때 사용하고, `connect` 는 remote endpoint 로 연결할 때
  사용한다.

## 피해야 하는 이름

`server()`, `client()`, `publisher()`, `subscriber()`, `router()`, `dealer()` 처럼 명사만
있는 이름은 피한다. 이런 이름은 단순 조회처럼 보이지만, framework option builder 에서는
대부분 역할을 활성화한다. 실제 동작이 활성화라면 `enableServer()` 또는
`enable_server()` 처럼 쓴다.

Discovery 자체는 `UseDiscovery` / `useDiscovery` / `use_discovery` 로 연다. 다만 그 안에서
Registry endpoint 를 추가할 때는 `Add(endpoint)`, `add(endpoint)`, `connectRegistry(endpoint)`
처럼 의미가 부족한 이름을 쓰지 않는다. Registry endpoint 는 반복 가능한 설정 값이므로
Discovery builder 의 `AddRegistryEndpoint` / `addRegistryEndpoint` /
`add_registry_endpoint` 로 추가한다.

metadata forwarding key 도 같은 규칙을 따른다. `Forward(key)` 또는 `forward(key)` 는 정책
실행처럼 보이므로 쓰지 않고, 전달 허용 목록에 값을 추가한다는 뜻이 드러나게
`AddForwardedMetadataKey` / `addForwardedMetadataKey` / `add_forwarded_metadata_key` 를 쓴다.

## 언어별 표기법

각 언어는 그 언어의 일반 표기법을 따른다.

| 언어 | 등록 예 | 역할 예 | discovery 예 |
|------|---------|---------------|--------------|
| .NET | `AddClientServerChannel` | `EnableServer` | `UseDiscovery(d => d.AddRegistryEndpoint(...))` |
| Java | `addClientServerChannel` | `enableServer` | `useDiscovery(d -> d.addRegistryEndpoint(...))` |
| Node | `addClientServerChannel` | `enableServer` | `useDiscovery().addRegistryEndpoint(...)` |
| C++ | `add_client_server_channel` | `enable_server` | `use_discovery().add_registry_endpoint(...)` |

Kotlin DSL 은 Java builder 를 감싼 얇은 표면이므로 Java 이름의 의미를 바꾸지 않는다.

## 설계 이유

이 규칙은 호출자가 builder 의 내부 상태 모델을 몰라도 코드를 읽을 수 있게 하기 위한
것이다. `add*` 와 `enable*` 를 구분하면 등록과 활성화가 분리되어 보이고, `bind` 와
`connect` 를 구분하면 endpoint 방향이 드러난다. 이 방식은 option builder 를 더 깊은
모듈로 만들고, 호출자에게 숨은 전제 지식을 덜 요구한다.
