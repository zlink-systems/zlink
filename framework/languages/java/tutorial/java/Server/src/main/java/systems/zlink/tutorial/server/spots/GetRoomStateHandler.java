package systems.zlink.tutorial.server.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.tutorial.shared.Contracts;

// The third type argument is the reply. This handler only reads.
public final class GetRoomStateHandler
    implements ZLinkSpotRequestHandler<GameRoom, Contracts.GetRoomState, Contracts.RoomState> {

    @Override
    public CompletionStage<Contracts.RoomState> handle(
        GameRoom room,
        Contracts.GetRoomState request) {
        return CompletableFuture.completedFuture(room.state());
    }
}
