# Round 195: Spot wire multipart scope 후보

## 저부하 분석

고부하 성능 실행은 다른 framework의 짧은 startup·reliability gate와 자원 경합을
피하기 위해 실행하지 않았다. 기존 1-peer Callgrind와 현재 source를 다시 대조했다.

- profile: `/home/hep7/.cache/zlink-core-validation/callgrind.spot-server.2470219`
- 처리한 request: 1,968개
- `zlink_mesh_reply()` 호출: 1,969회
- `wire_submit_reply()` inclusive instruction: 10,930,194
- `handle_spot_data()` inclusive instruction: 7,955,174

일반 1-part reply도 wire에서는 envelope와 payload의 2-part 메시지다. 기존 구현은
Mesh가 두 part 전체를 이미 소유하는데도 각 frame을 공개
`zlink_send_part_rid()`에 따로 넘겼다. 첫 frame은 public part helper의 handle state와
send scope를 만들고, 다음 frame은 같은 state를 다시 조회해 mutex를 획득한 뒤
family·flags·routing ID를 비교한다. 이 상태 기계는 서로 다른 public API 호출 사이의
multipart 수명을 보존할 때 필요하지만, 한 함수 안에서 전체 wire message를 보내는
Mesh에는 필요하지 않다.

두 대안을 비교했다.

1. 공용 multipart helper에 Mesh 전용 fault-injection callback을 추가하면 기존
   transaction 코드를 재사용할 수 있지만 test 정책이 socket 공용 모듈로 누출된다.
2. Mesh wire 모듈이 ROUTER send scope 하나를 열고 첫 envelope만 routed send로,
   나머지 metadata·payload는 같은 scope의 연속 frame으로 보낸다. 기존
   post-envelope allocation fault와 rollback 경계도 Mesh 안에 유지할 수 있다.

두 번째 대안을 후보로 적용했다. 공개 API, wire frame 순서, payload reference count,
부분 multicast 결과와 backpressure 의미는 바꾸지 않았다. 실패 시에는 기존처럼 열린
ROUTER multipart를 rollback하고 errno를 보존한다.

## 저부하 correctness gate

별도 Debug build
`/home/hep7/.cache/zlink-core-validation`에서 다음 세 integration suite를 순차
실행했다.

```text
test_mesh_node_basic              PASS (14 cases)
test_mesh_peer_admission          PASS (24 cases)
test_mesh_lifecycle_contracts     PASS (14 cases)
test_mesh_stress                  PASS (3 cases)
```

총 55개 case가 통과했다. 앞의 세 suite는 17.02초, stress suite는 0.08초가
걸렸다. 여기에는 envelope `MORE` 전송 직후 강제
allocation failure를 발생시키고 다음 publish가 정상 전송되는 rollback 회귀,
blocking backpressure 중 shutdown, remote request/reply, Spot direct request/reply,
multicast와 transfer가 포함된다.

metadata frame도 같은 scope에서 전송되는지 확인하기 위해 remote Spot direct
request에 canonical metadata를 추가하고 수신 byte를 exact 비교했다. 집중 실행은
1/1 통과했다. 변경 뒤 peer suite 전체 재실행에서는 24개 중 이 metadata 회귀를
포함한 23개가 통과했고, 기존에 간헐적으로 round-1·round-2 lifecycle generation을
같게 관측하는 `test_peer_drain_and_reconnect`만 실패했다. 해당 test 단독 재실행은
통과했다. 같은 기존 flake는 Round 191과 194에서도 기록됐으며 이 후보의 wire frame
경계와는 분리한다.

## 판정

이 후보는 source에서 불필요한 public part-helper 상태 경계를 제거했고 focused
correctness는 통과했지만, 처리량 개선은 아직 측정하지 않았다. 따라서 유지 판정도
반려 판정도 하지 않는다. coordinator가 고부하 실행을 재개한 뒤 안정 runtime과 같은
tcp·64바이트·100 peer·5초 paired 1회로 세 Spot 패턴의 절대 처리량과 비율, 지연,
pending queue와 drop을 비교한다. 공통 개선이 없으면 후보와 이 후보에만 필요한
변경을 원복한다.

version, package와 timeout은 변경하지 않았고 공식 `core/build` runtime도 다시 만들지
않았다. 공식 runtime SHA-256은 계속
`671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`다.
