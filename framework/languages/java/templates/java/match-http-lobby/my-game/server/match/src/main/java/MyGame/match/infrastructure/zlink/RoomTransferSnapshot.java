package MyGame.match.infrastructure.zlink;

import MyGame.match.domain.RoomRoster;
import MyGame.match.domain.RoomState;

public record RoomTransferSnapshot(RoomState.RoomSnapshot game, RoomRoster.RosterSnapshot roster) {}
