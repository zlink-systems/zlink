# 스펙 개정안 — #259 completion 예약 상한

작성 2026-09-12, 2026-09-12 전면 수정.

**결론부터 — 새 규칙을 더하지 않는다. §11이 덧붙인 규칙을 지운다.**

---

## 1. 문제

`01-submit-and-completion.ko.md` §11의 마지막 두 문단이 지금 이렇다.

> … 진행 중 operation과 dispatcher에서 대기·실행 중인 callback을 합친 수는 **4,096개를 넘지
> 않으므로** callback queue가 제한 없이 증가하지 않는다.
>
> **예약할 자리가 없으면 request를 보내기 전에 `CapacityExceeded`로 거부한다.** 한 번 수락한
> operation의 completion enqueue에는 거부하거나 버리는 경로가 없다.

구현이 이 문장을 그대로 따른다.
`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1389`

```cpp
if (!start->registered) {
    throw framework_exception_t (
      framework_error_kind_t::capacity_exceeded,
      "raw mesh request completion capacity is exhausted");
}
```

send 경로 `:1533`, `:1565`도 같다. **C++ request 벤치가 전부 이 예외로 죽는다.**

## 2. 이 문단은 기존 스펙 두 곳과 충돌한다

### 2.1 04 §1 — capacity authority는 정확히 둘이다

`04-application-job-queue-and-backpressure.ko.md` §1

> | Authority | 제한하는 것 |
> |---|---|
> | Core byte HWM | Core application-direction queue가 현재 보유한 physical-frame charge |
> | Application job queue | Host가 callback 시작 전에 수용한 application job permit 수 |

> **Core byte HWM과 Application job queue는 설정, profile, 단위, 계상 경계와 관측값을 공유하지
> 않는다.** 한 authority의 값을 다른 authority의 counter로 복사하면 책임이 겹친다.

§2가 그 값의 소유자도 정한다 — **`MaxQueuedApplicationJobs`가 "Host의 정확한 job permit 상한"** 이다.

**§11의 4,096은 §1이 인정하지 않는 세 번째 counter다.** "쌓인 job 개수의 마지노선"은 이미
Application job queue가 소유한다. 같은 것을 두 번 정의한 것이다.

### 2.2 04 §3 — 포화를 reject로 바꾸는 것은 이미 금지돼 있다

§3의 "다음 방식은 허용하지 않는다" 목록

> - permit 없이 먼저 receive한 뒤 별도 counter만 증가시키기
> - retained-credit lease나 Framework byte-HWM으로 Core HWM을 다시 구현하기
> - **포화를 reject, drop, fixed-delay polling 또는 busy spin으로 바꾸기**
> - spec이 소유하지 않는 unbounded 또는 hidden side backlog에 record 보관하기

§8도 같은 말을 반복한다 — "이미 수락된 record를 실행 queue가 용량을 이유로 거절하면 **§3이
금지한 '포화를 reject로 바꾸기'** 가 된다."

**§11의 `CapacityExceeded` 거부가 정확히 이 금지 위반이다.**

## 3. 포화가 일어나면 실제로 무슨 일이 일어나야 하나 — 전부 기존 스펙이다

새로 만들 것이 없다. 04 §6과 §1이 이미 전 과정을 소유한다.

| 단계 | 동작 | 소유 조항 |
|---|---|---|
| permits in use가 **pause 경계(기본 80 %)** 도달 | 지원 socket에 `PAUSED` 절대 상태 적용 → peer가 이 host로 보내는 것을 멈춘다 | 04 §6 |
| peer가 멈춘다 | — | 04 §6 |
| 이미 날아온 것 때문에 **100 %** 도달 | **더 이상 receive하지 않는다.** permit을 얻은 뒤에만 receive·claim하므로 자동이다 | 04 §3 순서 2 |
| recv가 멈추면 | Core queue가 차고 **TCP 흐름 제어**로 send 쪽 소켓이 **HWM**에 걸린다 | 04 §1 "Core byte HWM은 전송 경로의 **마지막 안전장치**" |
| send 쪽 | Core의 기존 send 동작 그대로 — `SNDTIMEO`까지 기다린다 | Core socket 스펙 |
| **resume 경계(기본 60 %)** 복귀 | `RUNNING` 전이 | 04 §6 |

**어느 단계에서도 메시지를 버리지 않는다.** 100 %를 넘겨도 이미 들어온 것은 전부 처리한다.
이것도 기존 조항이다.

- 04 §10 — "Core receive byte HWM이 찼을 때 sender까지 backpressure가 전달되며 **record를
  버리지 않는다**."
- 04 §3 — 금지 목록의 "포화를 reject, **drop**, fixed-delay polling 또는 busy spin으로 바꾸기".
- 04 §3 순서도 — permit 대기가 cancel·close·shutdown으로 끝나는 것은 "**cancellable wait
  종료(reject·drop 아님)**".
- 04 §8 — "한도 종류에 따라 terminal 의미를 구분하고 **조용히 버리지 않는다**."

포화는 **유입 속도를 늦추는 것**이지 이미 받은 일을 없애는 것이 아니다. 그래서 100 % 경로도
"버린다"가 아니라 "recv를 멈춘다"이고, 멈춘 결과가 TCP와 Core HWM을 통해 sender에게 전달된다.

**100 % 경로는 안전장치다.** 보통은 80 %에서 보낸 `PAUSED`로 상대가 멈추므로 거기까지 가지 않는다.

Framework가 Core에 주는 feedback은 04 §2가 이미 하나로 못박았다.

> **Framework job pressure가 Core에 주는 feedback은 지원 socket에 적용하는 `RUNNING`·`PAUSED`
> receive-flow 절대 상태 하나뿐이다.**

## 4. 개정 문안 — §11의 두 문단을 아래로 교체한다

> Terminal winner가 진행 중 호출 표의 항목을 꺼낸 뒤 dispatcher admission에 실패하면
> application completion을 잃는다. 따라서 operation을 수락할 때 completion dispatcher 자리도
> 함께 예약한다. 이 예약은 callback이 반환할 때까지 유지한다.
>
> **이 예약은 별도 capacity authority가 아니다.** 진행 중 operation과 dispatcher에서 대기·
> 실행 중인 callback은 [Application Job Queue와 backpressure §1](../../01-execution/04-application-job-queue-and-backpressure.ko.md#1-두-독립된-capacity-authority)의
> **Application job queue permit을 함께 소비한다.** 상한은 같은 문서 §2의
> `MaxQueuedApplicationJobs`이며 **host가 하나로 소유한다** — host 안의 MeshNode 수,
> ClientServer channel 수, connection 수와 무관하게 하나를 공유하고, MeshNode별이나
> connection별로 나누어 갖지 않는다. 집계 대상은 completion을 등록하는 모든 Messaging
> Request다. 따라서 callback queue가 제한 없이 증가하지 않는다.
>
> **포화는 거부가 아니라 흐름 제어로 처리한다.** 같은 문서 §6의 receive-flow 절대 상태가
> 유일한 제어 지점이며, §3이 금지한 대로 포화를 reject·drop으로 바꾸지 않는다. **permit 상한을
>넘긴 상태에서도 이미 수신한 record는 하나도 버리지 않고 모두 처리한다** — 포화는 유입 속도를
> 늦출 뿐이며, 그 수단은 receive를 멈추어 Core HWM과 transport 흐름 제어가 sender에게
> backpressure를 전달하게 하는 것이다. 한 번 수락한 operation의 completion enqueue에도
> 거부하거나 버리는 경로가 없다.

**지우는 것**: "4,096개를 넘지 않으므로", "예약할 자리가 없으면 request를 보내기 전에
`CapacityExceeded`로 거부한다."

**더하는 것**: 없다. 기존 조항을 가리키기만 한다.

## 5. 폐기한 안 — 기록해 둔다

이전 판이 제안했다가 **전부 철회**한다. 셋 다 기존 스펙과 충돌하거나 규칙을 늘린다.

| 폐기한 안 | 왜 틀렸나 |
|---|---|
| 자리가 없으면 **absolute deadline까지 대기** 후 `DeadlineExceeded` | 04 §8이 "**Request는** caller가 결과를 받아 판단할 수 있으므로 **bounded queue 확보를 기다리지 않는다**"고 이미 규정한다. 3단계 backpressure는 send·publish·one-way 전용이다 |
| §6 pressure 입력에 **"두 사용량 중 높은 비율"** 규칙 추가 | 두 counter가 아니라 하나다. §1의 `permits in use` 공식이 이미 소유한다 |
| 4,096을 **별도 상한**으로 두고 host 단위만 명시 | §1이 인정하지 않는 세 번째 authority를 정당화하는 것이 된다 |

이전 판의 "completion 예약을 receive-flow로 전파하면 자기 교착"이라는 주장도 근거가 없었다.
RouteMesh reply는 Completion pipe의 HWM-free admission을 쓰고(`core/doc/spec/core/socket/07-router.ko.md:441`),
ClientServer는 ROUTER가 DEALER에게 요청을 보낼 수 없어 DEALER가 `PAUSED`를 걸 이유 자체가 없다.

## 6. 4언어 적용

| 언어 | 해야 할 일 |
|---|---|
| C++ | `raw_mesh_node_owner.cpp:1389,1533,1565`의 `capacity_exceeded` throw 제거. 예약을 job queue permit으로 통합 |
| .NET · Java · Node | 별도 completion counter가 있으면 job queue permit으로 통합. ClientServer 경로가 집계에 드는지 확인 |

## 7. 절차

1. 위 §4 문안 확정 (사용자 승인)
2. `scripts/verify-framework-doc-contracts.sh`
3. codex 리뷰
4. 4언어 적용 + 회귀 테스트
5. C++ request 벤치 5쌍 완주 → #259 해소
6. gRPC 포함 전체 비교표
