[한국어](10-xsub.ko.md) | English

[Reference index](README.en.md)

# 10. XSUB

An extended subscriber with subscription forwarding upstream. XSUB exposes exactly the same
entry points as SUB — `zlink_set_sub_option`/`zlink_get_sub_option`,
`zlink_set_subscription`/`zlink_unset_subscription`, `zlink_subscribe_part`, and
`zlink_subscription_at` — with no XSUB-specific addition. This category exists only so XSUB has
a place in the taxonomy; see the [SUB category](08-sub.en.md) for every entry, and the
[XSUB specification](../spec/core/socket/05-xsub.en.md) for the exact contract (identical in
substance to the SUB specification, restated for the XSUB subject).

The one distinction from SUB is architectural rather than a different entry point: XSUB forwards
subscribe/unsubscribe messages upstream (to a paired XPUB, typically through a proxy), where SUB
does not expose that forwarding. No additional function reflects this — it is a wire-level
behavior of the socket type, not part of its public API.
