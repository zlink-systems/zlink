package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

/**
 * Optional base/delta capture capability for a Spot relocation adapter.
 *
 * <p>When the adapter class registered with {@code preserveStateWith(...)}
 * implements this interface, the Framework captures a base snapshot with
 * {@link #captureBase} at a turn boundary before the source admission seal,
 * transfers it ahead of time, and after the seal transfers only the delta
 * produced by {@link #captureDelta}. The target restores the base snapshot
 * with {@link #restoreBase} and applies the delta with {@link #applyDelta}.
 * The meaning of a delta is owned by the application. A failed
 * {@link #applyDelta} discards the instance and restarts from
 * {@link #restoreBase} on a fresh instance.
 */
public interface ZLinkSpotBaseDeltaRelocationAdapter<TSpot>
    extends ZLinkSpotRelocationAdapter<TSpot> {
    CompletionStage<byte[]> captureBase(
        TSpot spot,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<byte[]> captureDelta(
        TSpot spot,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> restoreBase(
        TSpot spot,
        byte[] base,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> applyDelta(
        TSpot spot,
        byte[] delta,
        ZLinkRelocationCancellation cancellation);
}
