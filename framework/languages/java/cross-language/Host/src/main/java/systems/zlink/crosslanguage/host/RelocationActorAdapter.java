package systems.zlink.crosslanguage.host;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

/** capture()/restore() so relocation preserves stateVersion and the
 * (possibly multi-chunk) application-state payload. */
public final class RelocationActorAdapter implements ZLinkActorRelocationAdapter<RelocationActor> {
    @Override
    public CompletionStage<byte[]> capture(
        RelocationActor actor, ZLinkRelocationCancellation cancellation) {
        byte[] state = actor.applicationState();
        // The cross-language fixture uses the .NET BinaryWriter-compatible
        // little-endian two-int header before the opaque state bytes.
        byte[] encoded = ByteBuffer.allocate(Integer.BYTES * 2 + state.length)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(actor.stateVersion())
            .putInt(state.length)
            .put(state)
            .array();
        return CompletableFuture.completedFuture(encoded);
    }

    @Override
    public CompletionStage<Void> restore(
        RelocationActor actor, byte[] payload, ZLinkRelocationCancellation cancellation) {
        ByteBuffer buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        int stateVersion = buffer.getInt();
        int length = buffer.getInt();
        if (length < 0 || length > buffer.remaining()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException("invalid relocation application state"));
        }
        byte[] state = new byte[length];
        buffer.get(state);
        actor.setStateVersion(stateVersion);
        actor.setApplicationState(state);
        return CompletableFuture.completedFuture(null);
    }
}
