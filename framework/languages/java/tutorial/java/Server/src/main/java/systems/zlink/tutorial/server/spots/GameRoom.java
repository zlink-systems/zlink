package systems.zlink.tutorial.server.spots;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.tutorial.shared.Contracts;

// --8<-- [start:spot-class]
// A room owns its own state and is addressed by a global SpotId. Messages sent
// to one room run one at a time, so the fields below need no synchronization.
//
// Every Java user Spot names the actor type it can admit. This room admits
// none, so it names the base type and rejects every join. Actors are a later
// chapter.
public final class GameRoom
        implements ZLinkSpot<ZLinkActor> {

    private final ZLinkSpotContext context;
    private final List<String> chat =
        new ArrayList<>();

    private String title = "untitled";

    public GameRoom(ZLinkSpotContext context) {
        this.context = context;
        // --8<-- [start:spot-handlers]
        // Handler classes are named here rather than scanned, the same way the
        // channel registrations name theirs. Each takes the target room as its
        // first argument.
        context.handlers().addHandler(
            PostChatHandler.class);
        context.handlers().addHandler(
            GetRoomStateHandler.class);
        // --8<-- [end:spot-handlers]
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    // Runs before the room accepts any message. Rejecting here means the create
    // call fails and no room exists. Omit this method to accept every request.
    @Override
    public
    CompletionStage<ZLinkSpotCreateResponse>
            onCreate(ZLinkMessage request) {
        var body = request.decode(
            Contracts.OpenRoom.class);
        title = body.title();
        var accepted =
            ZLinkSpotCreateResponse.accept();
        return CompletableFuture
            .completedFuture(accepted);
    }

    // No actor ever joins this room, so the three membership callbacks below say
    // so and do nothing else.
    @Override
    public
    CompletionStage<ZLinkSpotActorJoinResult>
    onActorJoin(
        String actorId,
        ZLinkMessage request) {
        var rejected =
            ZLinkSpotActorJoinResult.reject();
        return CompletableFuture
            .completedFuture(rejected);
    }

    @Override
    public CompletionStage<Void>
            onJoinedActor(ZLinkActor actor) {
        return CompletableFuture
            .completedFuture(null);
    }

    @Override
    public CompletionStage<Void>
            onLeaveActor(ZLinkActor actor) {
        return CompletableFuture
            .completedFuture(null);
    }

    void append(String line) {
        chat.add(line);
    }

    Contracts.RoomState state() {
        return new Contracts.RoomState(
            title, List.copyOf(chat));
    }
}
// --8<-- [end:spot-class]
