# Kotlin AutomaticTurnDispatch porting inventory

이번 작업에서 실제 수정한 Kotlin counterpart 파일은 다음 두 개다.

| 파일 | 변경 |
|---|---|
| `Server/Delay/src/main/java/.../DelayApplication.java` | `server().listen(...)`과 three-argument request handler 등록으로 교체 |
| `Server/Delay/src/main/java/.../DelayHandler.java` | 현재 `ZLinkMessageContext` 사용 |

전체 Kotlin fixture compile은 `Server/Play`를 포함한 기존 source에서 삭제된 context와 구형
factory/getOrCreate/join result/channel API 오류로 실패한다. Java fixture의 성공한 compile과
Kotlin compile blocker를 섞어 보고하지 않는다.
