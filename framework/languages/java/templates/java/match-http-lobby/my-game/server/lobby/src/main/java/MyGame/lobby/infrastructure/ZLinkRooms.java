package MyGame.lobby.infrastructure;

import MyGame.*;
import MyGame.lobby.application.RoomProvisioner;

import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class ZLinkRooms implements RoomProvisioner {
    private final ZLinkSpotManager spots;
    private final NodeConfig config;

    public ZLinkRooms(ZLinkSpotManager spots, NodeConfig config) {
        this.spots = spots;
        this.config = config;
    }

    public CompletionStage<MatchResponse> prepare(String roomId) {
        return spots.getOrCreate(roomId, Names.ROOM)
                .inMesh(Names.MESH)
                .submit()
                .thenApply(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Room creation rejected");
                            return new MatchResponse(
                                    created.spot().spotId(), config.sessionEndpoint());
                        });
    }
}
