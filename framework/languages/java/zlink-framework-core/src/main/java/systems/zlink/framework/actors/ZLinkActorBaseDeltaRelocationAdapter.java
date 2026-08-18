package systems.zlink.framework.actors;

import java.util.concurrent.CompletionStage;

/**
 * Optional base/delta capture capability for a relocation adapter.
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
public interface ZLinkActorBaseDeltaRelocationAdapter<TActor extends ZLinkActor>
    extends ZLinkActorRelocationAdapter<TActor> {
    CompletionStage<byte[]> captureBase(
        TActor actor,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<byte[]> captureDelta(
        TActor actor,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> restoreBase(
        TActor actor,
        byte[] base,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> applyDelta(
        TActor actor,
        byte[] delta,
        ZLinkRelocationCancellation cancellation);
}
