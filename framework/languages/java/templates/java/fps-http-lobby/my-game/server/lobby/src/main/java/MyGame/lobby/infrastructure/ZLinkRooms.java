package MyGame.lobby.infrastructure;

import MyGame.*;
import MyGame.lobby.application.RoomProvisioner;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.*;

import java.util.List;
import java.util.concurrent.*;

public final class ZLinkRooms implements RoomProvisioner {
    private final ZLinkSpotManager spots;
    private final ZLinkRouteClient routes;
    private final NodeConfig config;

    public ZLinkRooms(ZLinkSpotManager spots, ZLinkRouteClient routes, NodeConfig config) {
        this.spots = spots;
        this.routes = routes;
        this.config = config;
    }

    public CompletionStage<MatchResponse> prepare(String roomId, List<String> members) {
        return spots.getOrCreate(roomId, Names.ROOM)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("Room creation rejected"));
                            String spotId = created.spot().spotId();
                            return routes.requestToSpot(spotId, new PrepareRoom(roomId, members))
                                    .submit(RoomPrepared.class)
                                    .thenApply(
                                            prepared -> {
                                                if (!roomId.equals(prepared.matchId())
                                                        || !spotId.equals(roomId))
                                                    throw new IllegalStateException(
                                                            "Prepared Room identity differs from the match");
                                                if (config.sessionEndpoint() == null
                                                        || config.sessionEndpoint().isBlank())
                                                    throw new IllegalStateException(
                                                            "Session endpoint is required");
                                                return new MatchResponse(roomId, config.sessionEndpoint());
                                            });
                        });
    }
}
