[한국어](EXAMPLES.ko.md)

[가이드 목록](README.ko.md) · [스타일 규약](STYLE.ko.md)

# 예제 코드 관리 규약 — drift 방지

가이드의 코드 예제는 시간이 지나면 실제 API와 어긋나기 쉽다(drift). 이 문서는
예제가 **컴파일·실행되는 샘플과 묶여 부패하지 않도록** 하는 규약을 정의한다.

> 원칙: 가이드의 코드는 손으로만 유지하지 않는다. 깨지면 빌드가 깨지도록 묶어서
> "문서가 조용히 틀려지는" 상황을 막는다.

## 1. 단일 출처 — `bindings/<lang>/samples/`

각 언어는 이미 실행 가능한 샘플을 가지고 있다.

- `bindings/dotnet/samples/` · `bindings/cpp/samples/` ·
  `bindings/java/samples/` · `bindings/node/samples/` 등.
- 각 바인딩에는 샘플 runner(`run_samples.sh` 등)가 있고, 일부는 CI smoke로 검증된다.

가이드 예제는 이 샘플을 **단일 출처**로 삼는다.

## 2. 두 가지 수준 (현실적 단계 적용)

### 수준 A — 1:1 대응 (현재 기준)

가이드의 각 주요 예제는 동일 동작을 하는 샘플과 **1:1로 대응**시키고, 가이드에
샘플 파일명을 명시한다.

- 예: 바인딩 가이드 `02-messaging`의 PAIR 예제 ↔ `samples/PairRecv`.
- 샘플이 CI 스모크로 검증되므로, API가 바뀌면 샘플 빌드가 깨져 신호를 준다.
- 가이드 코드와 샘플이 **표현은 달라도 호출하는 공개 API는 같아야** 한다.

### 수준 B — 명명된 스니펫 추출 (목표)

샘플 파일에 명명된 스니펫 영역을 두고 가이드가 그 영역을 추출해 싣는다.

```csharp
// #region guide:pair-send
client.Send().Message(Message.From("PING")).Submit();
// #endregion
```

- 빌드 시 `#region guide:<name>` 블록을 가이드의 대응 코드 블록에 주입.
- 이러면 가이드 코드가 **샘플에서 자동 생성**되어 drift가 구조적으로 불가능해진다.
- 현재 문서 빌드는 `--8<--` snippet 경로 지시자(따옴표로 감싼 `path:section` 인자)를 쓰며, `#region guide` 자동 추출은
  추출 스크립트와 CI 단계가 필요하므로 후속 작업으로 둔다.

## 3. 검증 (회귀)

- 샘플 runner로 빌드·실행해 검증한다 — 가이드가 참조하는 샘플이 깨지면 잡힌다(일부는 CI smoke).
- (권장) 가이드 코드 블록이 참조하는 샘플 파일이 실제로 존재하는지 검사하는
  링크 체크를 문서 회귀 테스트에 추가한다.
- 추측 API 금지: 가이드에 쓴 메서드는 해당 바인딩의 `Contracts/`·소스에 실재해야
  한다([스타일 규약 §7](STYLE.ko.md)).

## 4. 작성자 체크리스트

가이드에 코드 예제를 추가·수정할 때:

- [ ] 호출한 API가 그 언어의 공개 contract에 실재하는가(지어내지 않았는가)?
- [ ] 대응하는 샘플(`samples/...`)이 있는가, 없으면 추가했는가?
- [ ] 값이 현실적인가(포트·심볼·금액 등 production-like)?
- [ ] 코어 가이드의 같은 시나리오와 일관된가([공유 시나리오](scenarios.ko.md))?

---

> 더 보기: [스타일 규약](STYLE.ko.md) · [공유 시나리오](scenarios.ko.md).
