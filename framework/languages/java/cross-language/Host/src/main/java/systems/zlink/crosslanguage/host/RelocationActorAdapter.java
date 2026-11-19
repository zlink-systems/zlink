package systems.zlink.crosslanguage.host;

import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/**
 * capture()/restore() so relocation preserves stateVersion and the (possibly multi-chunk)
 * application-state payload.
 */
public final class RelocationActorAdapter implements ZLinkActorRelocationAdapter<RelocationActor> {
    private static final java.util.concurrent.ConcurrentHashMap<String, RestoreGate> RESTORE_GATES =
            new java.util.concurrent.ConcurrentHashMap<>();

    static RestoreGate holdRestore(String actorId) {
        RestoreGate gate = new RestoreGate(actorId);
        if (RESTORE_GATES.putIfAbsent(actorId, gate) != null)
            throw new IllegalStateException("restore observation already registered");
        return gate;
    }

    static final class RestoreGate {
        private final String actorId;
        private final CompletableFuture<Void> entered = new CompletableFuture<>();
        private final CompletableFuture<Void> released = new CompletableFuture<>();

        private RestoreGate(String actorId) {
            this.actorId = actorId;
        }

        CompletionStage<Void> entered() {
            return entered;
        }

        void release() {
            RESTORE_GATES.remove(actorId, this);
            released.complete(null);
        }
    }

    @Override
    public CompletionStage<byte[]> capture(
            RelocationActor actor, ZLinkRelocationCancellation cancellation) {
        byte[] state = actor.applicationState();
        // The cross-language fixture uses the .NET BinaryWriter-compatible
        // little-endian two-int header before the opaque state bytes.
        byte[] encoded =
                ByteBuffer.allocate(Integer.BYTES * 2 + state.length)
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
        RestoreGate gate = RESTORE_GATES.get(actor.actorId());
        if (gate == null) return CompletableFuture.completedFuture(null);
        gate.entered.complete(null);
        return gate.released;
    }
}
