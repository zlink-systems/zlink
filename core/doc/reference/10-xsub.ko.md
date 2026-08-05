한국어 | [English](10-xsub.en.md)

[레퍼런스 목차](README.ko.md)

# 10. XSUB

구독을 upstream으로 전달하는 확장 subscriber다. XSUB은 SUB과 정확히 같은 진입점 —
`zlink_set_sub_option`/`zlink_get_sub_option`,
`zlink_set_subscription`/`zlink_unset_subscription`, `zlink_subscribe_part`,
`zlink_subscription_at` — 을 노출하며 XSUB 전용 추가는 없다. 이 category는 taxonomy 안에
XSUB의 자리를 마련하기 위해서만 존재한다 — 모든 항목은 [SUB category](08-sub.ko.md)를,
정확한 계약은 [XSUB 스펙](../spec/core/socket/05-xsub.ko.md)(SUB 스펙과 실질적으로 동일하며
XSUB subject로 다시 서술됨)을 참고한다.

SUB과의 유일한 차이는 다른 진입점이 아니라 아키텍처적인 것이다 — XSUB은 구독/구독 해제
메시지를 upstream(보통 proxy를 거쳐 짝이 되는 XPUB)으로 전달하지만, SUB은 그런 전달을
노출하지 않는다. 이를 반영하는 추가 함수는 없다 — 이는 socket 타입의 wire-level 동작이지
공개 API의 일부가 아니다.
