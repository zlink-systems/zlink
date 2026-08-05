# JVM public API snapshot

`java.api.sha256`와 `kotlin.api.sha256`는 source jar와 temporary Maven package에서
동일한 public JVM surface를 생성한 뒤 그 결과의 SHA-256을 기록한다. 비교 명령은
`framework/languages/java/scripts/verify_api_snapshot.sh`이다.

이 gate는 source와 package가 서로 다른 API를 내보내는 경우를 통과로 처리하지 않는다.
snapshot 파일이 없거나 `sha256=` 값이 없으면 `api_snapshot_not_found`로 실패한다.
public surface를 의도적으로 바꿀 때만 source/exact interface와 clean consumer를 먼저
검토한 뒤 candidate 결과를 새 snapshot으로 반영한다.
