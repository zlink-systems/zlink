# 언어별 Framework 공개 계약

이 디렉토리는 Framework server package의 공통 동작이 각 언어의 public API에서
어떤 정확한 형태로 제공되는지 정의한다. 여기에 기록한 signature는 해당 언어
구현과 contract test가 따라야 하는 정식 계약이다.

Client package의 public interface는 이 디렉토리에서 정의하지 않는다. Stream
connector는 [언어별 Stream connector 계약](../../stream-connector/README.ko.md),
HTTP client는 [언어별 HTTP client 계약](../../http-client/README.ko.md)이
각각 소유한다.

언어에 공통인 동작은 [공통 스펙](../../README.ko.md)이 정의하고, 계약을 변경하는
절차는 [공개 계약 관리](../../00-public-contract-governance.ko.md)를 따른다.

| 언어 | 공개 계약 |
|------|-----------|
| `.NET` | [dotnet](dotnet/README.ko.md) |
| Java | [java](java/README.ko.md) |
| Kotlin | [kotlin](kotlin/README.ko.md) |
| Node.js framework | [node](node/README.ko.md) |
| C++ | [cpp](cpp/README.ko.md) |

언어별 스펙은 서로의 시그니처를 복사하는 문서가 아니다. 같은 공통 동작을 해당
언어 사용자가 자연스럽게 사용할 수 있는 public contract로 고정한다.
