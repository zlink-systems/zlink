# ZLink Framework Node.js 공개 계약

이 디렉토리는 Node.js framework가 제공해야 하는 **정식 public contract**를 소유한다.
package export, public declaration과 contract test는 이 계약을 따라야 한다.

| 번호 | 문서 | 범위 |
|---|------|------|
| `01` | [시스템 구조](01-system-structure.ko.md) | 패키지 구조·배포, NestJS 모듈 등록, DI, Instance Spot을 포함한 lifecycle과 startup validation |
| `interfaces` | [공개 인터페이스 목차](interfaces/README.ko.md) | 범주별 TypeScript declaration, Location Store, maintenance와 routing ID 자동 할당 |

**기능의 의미와 동작 규칙은 [공통 스펙](../../../README.ko.md)이 소유한다.** 이 디렉토리는 그 의미가
이 언어에서 갖는 **정확한 public API**만 고정한다.

Host relocation은 mode를 반드시 지정한다. Planned maintenance는 source와 같은 application version을,
rolling update는 호출자가 지정한 더 높은 application version과 정확히 일치하는 target만 사용한다.
Host 종료는 별도 `shutdown()` operation이다.

## 취소 인자

Node.js public interface는 **일반 handler에 취소 인자를 자동으로 추가하지 않는다.** 호출자가
중단할 수 있어야 하는 request 대기·연결·종료 같은 장기 작업은 Node 관례에 따라 optional
`AbortSignal`을 사용한다. 정확한 적용 대상은 인터페이스 시그니처가 고정한다.
