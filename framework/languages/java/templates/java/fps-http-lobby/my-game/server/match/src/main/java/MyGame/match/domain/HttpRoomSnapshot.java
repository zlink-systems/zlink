package MyGame.match.domain;

public record HttpRoomSnapshot(RoomState.RoomSnapshot game, RoomRoster.RosterSnapshot roster) {}
