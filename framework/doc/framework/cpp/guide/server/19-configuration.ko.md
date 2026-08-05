---
title: "19. Configuration · C++"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 18. DI 컨테이너](18-di-container.ko.md) | [다음: 20. HTTP Hosting](20-http-hosting.ko.md)
<!-- framework-adapter-nav:end -->

# 19. Configuration

> **이 장의 계약 소유 문서** — [C++ configuration과 host 공개 계약](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)이
> 다룬다. 이 챕터는 CLI·환경변수·JSON 파일에서 설정 값을 읽어 오는 방법을 설명한다.
> 무엇을 설정할 수 있는지는 [16. Options](16-options.ko.md)가 모은다.

endpoint·포트·동작 플래그를 코드에 하드코딩하지 않고 CLI/환경변수/JSON 파일에서
읽는 방법을 다룬다. 진입점은 `app.config()`(`config_builder_t`)다.

## 1. 모델: 평탄한 key-value

모든 소스는 하나의 평탄한 모델로 합쳐진다. 키는 점(`.`)으로 구분된 경로다.

```text
sample.topology.apiHttpEndpoint = http://0.0.0.0:8080
sample.host.keepRunning         = true
environment.name                = production
```

## 2. 소스 로딩

```cpp
auto &config = app.config ();
config.load_json ("config/match-api.json");            // 없으면 예외
config.load_json ("config/local.json",                 // 없어도 통과
                  zlink::framework::optional_t::yes);
config.load_env ("MATCH_API__");                       // prefix 매칭 env
config.load_cli (argc, argv);                          // --key=value
```

각 소스의 키 매핑 규칙:

| 소스 | 입력 | 모델 키 |
|------|------|---------|
| `load_json` | `{"sample":{"topology":{"apiEndpoint":"tcp://..."}}}` | 중첩 객체를 평탄화: `sample.topology.apiEndpoint` |
| `load_env(prefix)` | `MATCH_API__sample__host__keepRunning=true` | prefix 제거 후 `__` → `.`: `sample.host.keepRunning` |
| `load_cli` | `--sample.host.keepRunning=true` | 그대로: `sample.host.keepRunning` |
| `load_cli` (값 없는 플래그) | `--verbose` | `verbose=true` |

JSON 문자열/숫자/불리언/null 값은 문자열 값으로 평탄화된다. 조회 측에서 필요한
타입으로 해석한다.

## 3. 우선순위

**나중에 로드한 소스가 같은 키를 덮어쓴다.** 관례적인 순서는
"파일 < 환경변수 < CLI"다 — 마지막에 로드한 CLI가 최종 승자.

```cpp
// 권장 순서: 기본 파일 → 환경별 파일 → env → cli
config.load_json ("config/match-api.json");
config.load_json ("config/match-api." + config.environment () + ".json",
                  zlink::framework::optional_t::yes);
config.load_env ("MATCH_API__");
config.load_cli (argc, argv);
```

bootstrap이 필요한 경우(예: `--config=<path>`로 파일 위치 자체를 받는 경우)
CLI를 먼저 한 번 읽고, 파일 로드 후 다시 CLI로 덮는다. TicTacToe 샘플의 실제
패턴:

```cpp
app.config ().load_cli (argc, argv);                       // ① --config 읽기
if (auto path = app.config ().model ().get ("config")) {
    app.config ().load_json (*path);                       // ② 지정 파일 로드
}
app.config ().load_env ("ZLINK_CPP_SAMPLE__").load_cli (argc, argv);   // ③ env < cli
```

## 4. 값 조회

### 단건 조회

```cpp
auto endpoint = app.config ().model ().get ("sample.topology.apiHttpEndpoint");
// std::optional<std::string>

bool keep_running =
  app.config ().model ().get ("sample.host.keepRunning").value_or ("false") == "true";
```

### section: prefix 묶음 조회

```cpp
auto section = app.config ().section ("sample.topology");
auto api = section.get ("apiEndpoint");          // sample.topology.apiEndpoint
auto registry = section.require ("registryRouterEndpoint");
// require는 없으면 framework_exception_t(request_protocol_error) throw
```

## 5. 타입 바인딩: bind<T>

설정 묶음을 struct로 받으려면 `static T bind(const configuration_section_t&)`를
구현한다 (`configuration_bindable` concept).

```cpp
struct topology_t
{
    std::string api_endpoint = "tcp://0.0.0.0:5555";
    std::string api_http_endpoint = "http://0.0.0.0:8080";

    static topology_t bind (const zlink::framework::configuration_section_t &section)
    {
        topology_t value;
        value.api_endpoint = section.get ("apiEndpoint").value_or (value.api_endpoint);
        value.api_http_endpoint =
          section.get ("apiHttpEndpoint").value_or (value.api_http_endpoint);
        return value;
    }
};
```

```cpp
// 섹션이 없으면 nullopt — 기본값으로 대체
auto topology = app.config ().bind<topology_t> ("sample.topology")
                  .value_or (topology_t{});

// 섹션이 반드시 있어야 하면
auto topology = app.config ().bind_required<topology_t> ("sample.topology");
```

bind한 struct를 DI에 singleton으로 올려 두면 핸들러가 주입받을 수 있다
([18장](18-di-container.ko.md)).

```cpp
options.services ().add_singleton<topology_t> (std::make_unique<topology_t> (topology));
```

## 6. 환경 이름

배포 환경 구분이 필요하면 환경 이름을 쓴다.

```cpp
app.config ().use_environment ("staging");

if (app.config ().is_environment ("production")) { /* ... */ }
auto suffix = app.config ().environment ();     // "staging"
```

## 7. 권장 패턴 정리

1. **bootstrap 함수 하나로 모은다** — `load_*` 순서가 곧 우선순위 정책이므로
   앱마다 흩어 놓지 않는다 (샘플의 `load_sample_configuration` 패턴).
2. **topology는 `bind<T>` struct로** — 문자열 키 조회를 코드 곳곳에 퍼뜨리지
   말고 한 곳에서 struct로 바인딩한 뒤 DI로 전달한다.
3. **기본값은 struct 멤버 초기화로** — `value_or` 체인이 자연스럽게 "설정이
   있으면 덮어쓰기"가 된다.
4. 필수 값은 `require`/`bind_required`로 — 빠진 설정은 부팅 시점에 시끄럽게
   실패하는 편이 낫다.

## 8. 관련 문서

- 정식 계약: [C++ configuration과 host 공개 계약](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)
- 설정할 수 있는 값 목록: [16. Options](16-options.ko.md)
- DI 등록: [18. DI 컨테이너](18-di-container.ko.md)
