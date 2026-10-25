package MyGame.lobby.infrastructure;

import MyGame.*;
import MyGame.lobby.application.RoomProvisioner;

import systems.zlink.framework.spots.*;
import systems.zlink.framework.channels.ZLinkRouteClient;

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
                                throw new IllegalStateException("Room creation rejected");
                            if (!roomId.equals(created.spot().spotId()))
                                throw new IllegalStateException("Created Room identity mismatch");
                            return routes.requestToSpot(roomId, new PrepareRoom(roomId, members))
                                    .submit(RoomPrepared.class);
                        })
                .thenApply(
                        prepared -> {
                            if (!roomId.equals(prepared.matchId()))
                                throw new IllegalStateException("Prepared Room identity mismatch");
                            return new MatchResponse(roomId, config.sessionEndpoint());
                        });
    }
}
