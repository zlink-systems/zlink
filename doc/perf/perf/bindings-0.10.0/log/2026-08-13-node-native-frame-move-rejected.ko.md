# Node native frame move 후보 결과

## 후보

single-part DONT_WAIT send에서 `Message`가 이미 보유한 native frame을 임시
`zlink_msg_t`로 copy하지 않고 move해 submit하고, backpressure면 원래 frame으로
되돌리는 후보를 검토했다. 성공 send가 Message ownership을 소비한다는 contract를
이용해 native reference-count copy를 줄이는 목적이었다.

## 검증

- Core: release `0.10.1`
- Linux Node add-on build: Linux Node `18.19.1`과 repository의 `node-gyp`로 통과
- ownership, multipart, HWM contract test: 통과
- 전체 Node test 중 `public_exports.test.js`의 ESM `require()` 실패는 Linux Node 18의
  기존 runner 호환 문제다. native frame 후보와 관련된 ownership test는 통과했다.

## 측정 결과

C 후 Node를 순차 실행했다. 공통 조건은 `MULTI_DEALER_DEALER`, tcp, clients `100`,
duration `2초`, runs `1`, message size `64 / 256 / 1024 / 4096 / 65536 / 131072B`,
balanced auto-HWM이다.

- C report: `/tmp/zlink-node-move-send-c/multi/report/perf_c_multi_linux_20260813_044459_node-move-send-c.txt`
- Node runner: Linux Node `18.19.1`, `--reuse-build`

Node runner는 64B, 256B, 1024B를 출력한 뒤 4KiB 이후 complete report를 생성하지
않았다. 따라서 C 대비 평균을 계산하지 않았고 후보의 성능 효과를 인정하지 않았다.
source와 Linux add-on을 기존 copy 경로로 원복했다.
