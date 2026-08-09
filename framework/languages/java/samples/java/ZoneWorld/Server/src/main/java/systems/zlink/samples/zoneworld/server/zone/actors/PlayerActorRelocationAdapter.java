package systems.zlink.samples.zoneworld.server.zone.actors;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

public final class PlayerActorRelocationAdapter implements ZLinkActorRelocationAdapter<PlayerActor> {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Override
    public CompletionStage<byte[]> capture(PlayerActor actor, ZLinkRelocationCancellation cancellation) {
        try {
            return CompletableFuture.completedFuture(JSON.writeValueAsBytes(new State(
                actor.x(), actor.y(), actor.zoneId(), actor.isBot(), actor.dirX(), actor.dirY(),
                actor.pendingX(), actor.pendingY(), actor.pendingZone(), actor.pendingJoin())));
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    @Override
    public CompletionStage<Void> restore(
        PlayerActor actor,
        byte[] state,
        ZLinkRelocationCancellation cancellation) {
        try {
            State restored = JSON.readValue(state, State.class);
            actor.restoreState(
                restored.x(), restored.y(), restored.zoneId(), restored.isBot(),
                restored.dirX(), restored.dirY(), restored.pendingX(), restored.pendingY(),
                restored.pendingZone(), restored.pendingJoin());
            return CompletableFuture.completedFuture(null);
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private record State(
        int x,
        int y,
        String zoneId,
        boolean isBot,
        int dirX,
        int dirY,
        int pendingX,
        int pendingY,
        String pendingZone,
        boolean pendingJoin) {
    }
}
