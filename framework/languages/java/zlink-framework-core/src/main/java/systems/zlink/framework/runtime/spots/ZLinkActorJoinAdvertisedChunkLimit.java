package systems.zlink.framework.runtime.spots;

/**
 * Extension point for the target's advertised relocation state chunk
 * receive limit that a follow-up wire schema change will attach to the
 * Actor Join Accepted reply tail (spec 15 §4.2: "승인 reply에는 relocation
 * payload를 나눠 보내는 단위인 relocation state chunk의 target 유효 수신
 * 상한을 함께 싣는다 — 이 값은 재계산에도 낮아지지 않는 안정 하한 기반의
 * 보수값이다").
 *
 * <p>This class computes the value; nothing writes it to the wire yet.
 * When the schema change lands, the Accepted reply encoder calls
 * {@link #conservativeReceiveChunkLimitBytes()} and appends the result to
 * the reply tail — no other change should be needed at that call site.
 */
final class ZLinkActorJoinAdvertisedChunkLimit {
    //  Same conservative floor spec 15 §4.2 already guarantees for the
    //  non-negotiated JoinEntrySpot Restore path (no admission round trip
    //  to negotiate a higher cap): 32 KiB encoded chunk size, safe for
    //  every deployment.
    private static final long CONSERVATIVE_RECEIVE_CHUNK_LIMIT_BYTES =
        32L * 1024;

    private ZLinkActorJoinAdvertisedChunkLimit() {
    }

    /**
     * The stable-floor advertised receive chunk limit for a Join Accepted
     * reply. Recomputation must never return a value lower than a value
     * already advertised for a still-open lease (spec 15 §4.2); today's
     * fixed conservative floor trivially satisfies that.
     */
    static long conservativeReceiveChunkLimitBytes() {
        return CONSERVATIVE_RECEIVE_CHUNK_LIMIT_BYTES;
    }
}
