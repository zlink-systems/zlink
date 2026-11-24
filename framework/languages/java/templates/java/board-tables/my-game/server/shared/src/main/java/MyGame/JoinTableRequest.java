package MyGame;

import systems.zlink.framework.ZLinkJsonOptional;

public record JoinTableRequest(String playerId, String tableId, @ZLinkJsonOptional boolean reconnect) {
    public JoinTableRequest(String playerId, String tableId) {
        this(playerId, tableId, false);
    }
}
