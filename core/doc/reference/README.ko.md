한국어 | [English](README.en.md)

[Core 스펙](../spec/core/README.ko.md) · [Core 가이드](../guide/01-overview.ko.md)

# ZLink Core 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 그 가이드는 managed 언어 framework의 fluent builder 체인을 대상으로 작성됐다. Core는
builder도 terminal도 없는 flat C ABI이므로, 항목마다 임의로 각색하지 않고 이 문서에서 매핑을 한 번
정한다.

## C API용 entry-unit 매핑

| Framework 레퍼런스 섹션 | Core 레퍼런스 섹션 | 여기서의 의미 |
|---|---|---|
| Heading | Heading | export된 함수 하나, 또는 함께 호출해야만 의미가 있는 긴밀한 함수 쌍·family(예: `zlink_ctx_set`/`zlink_ctx_get`, `zlink_poller_add`/`_modify`/`_remove`) |
| 코드 예제 | 코드 예제 | 함수 signature를 보여주는 최소 C 호출 |
| **옵션** | **Parameters** | 함수의 매개변수, 적용 가능한 flag 값, 함수가 읽거나 쓰는 option-struct field — builder 체인이 없으므로 그 자체가 아니다 |
| **완료 결과** | **Return과 errno** | 결과별 반환값의 의미와, Core가 설정하는 `errno`·typed-result 값. Core에는 비동기 완료가 없다 — 내부적으로 비동기 I/O를 시작하더라도 호출자 관점에서는 이 문서의 모든 호출이 동기다 |
| **선택 기준** | **선택 기준** | 취지는 그대로 유지 |

Core의 실패 모델은 framework의 단일 `FrameworkException.kind`와 다르다 — 각 API family가 자신의
typed result enum(`zlink_config_result_t`, `zlink_connect_result_t`, `zlink_submit_result_t` 등)을
반환하고, `zlink_errno()`가 같은 스레드의 상세 원인을 담는다. 아래 "Errors, results, and version"
category가 framework의 error-kind 대응표에 해당하는 이 레퍼런스의 대응 문서다 — 한 번 읽어두면 각
항목의 "Return과 errno" 섹션은 그 항목이 실제로 만드는 값만 이름으로 언급하면 된다.

## 로케일 관례

Framework의 interface 파일과 달리, Core spec 문서는 전부 en·ko 둘 다 이미 존재한다. Core의 원본
로케일은 English다(en이 정본, ko가 번역본) — framework interface 관례와 반대다. 그래서:

- `.en.md`를 먼저, `.ko.md`를 나중에 쓴다.
- 레퍼런스 파일의 spec 인용 링크는 **같은 로케일의** spec 파일을 가리킨다(`habitat.en.md` →
  `../spec/core/01-context.en.md`, `habitat.ko.md` → `../spec/core/01-context.ko.md`). 이 트리
  어디에도 "(Korean-only)" 표기가 없다 — Korean-only 원본이 없기 때문이다.

## Category

Core의 공개 표면은 framework의 정제된 8개 category보다 훨씬 세분화되어 있다 — 9개 공통 계약 챕터와
8개 socket 타입에 걸쳐 exported 함수가 약 90개인 raw C ABI다. Spec 챕터 중 둘은 자체 entry point가
없어 레퍼런스 category가 아니다:
[공개 계약 거버넌스](../spec/core/00-public-contract-governance.ko.md)(문서 정책이지 API가
아님)와 [Runtime boundary](../spec/core/09-runtime-boundary.ko.md)(범위 선언 — 역할 경계가
필요하면 [Core internals](../internals/architecture.ko.md)를 대신 참고한다).
[Events](../spec/core/05-events.ko.md)는 세 event family 사이의 관계를 정리하는 카탈로그일 뿐 자체
함수를 노출하지 않는다 — 그 내용은 별도 category가 되는 대신 아래 Polling과 Socket monitor의 "선택
기준" 산문에 흡수했다.

| Category | 상태 | 대응 spec |
|---|---|---|
| [Context](01-context.ko.md) | 작성 완료 | 01-context |
| [Message](02-message.ko.md) | 작성 완료 | 02-message |
| [Socket lifecycle](03-socket-lifecycle.ko.md) | 작성 완료 | socket/README §Functions(create/bind/connect/disconnect/close) |
| [Socket options and identity](04-socket-options.ko.md) | 작성 완료 | socket/README §Socket Options, §Dedicated Functions |
| [Raw receive](05-raw-receive.ko.md) | 작성 완료 | socket/README §Receive Model Summary, 03-errors §4 |
| [PAIR](06-pair.ko.md) | 작성 완료 | socket/01-pair |
| [PUB](07-pub.ko.md) | 작성 완료 | socket/02-pub |
| [SUB](08-sub.ko.md) | 작성 완료 | socket/03-sub |
| [XPUB](09-xpub.ko.md) | 작성 완료 | socket/04-xpub |
| [XSUB](10-xsub.ko.md) | 작성 완료 | socket/05-xsub |
| [DEALER](11-dealer.ko.md) | 작성 완료 | socket/06-dealer |
| [ROUTER](12-router.ko.md) | 작성 완료 | socket/07-router |
| [STREAM](13-stream.ko.md) | 작성 완료 | socket/08-stream |
| [Socket monitor](14-socket-monitor.ko.md) | 작성 완료 | 07-monitoring, 05-events |
| [Polling and pollers](15-polling.ko.md) | 작성 완료 | 06-polling, 05-events |
| [Timers](16-timers.ko.md) | 작성 완료 | 08-utilities §Timers |
| [Utilities](17-utilities.ko.md) | 작성 완료 | 08-utilities §Atomic Counter, §Stopwatch, §Miscellaneous |
| [Errors, results, and version](18-errors.ko.md) | 작성 완료 | 03-errors, 04-errno-map |

파일명은 작성되는 순서대로 위 번호의 `NN-slug.en.md`/`NN-slug.ko.md`를 따른다 — 이 표는 파일이
실제로 있을 때만 링크를 걸어서 끊어진 링크를 만들지 않는다.

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.
