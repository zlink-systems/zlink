package MyGame.match.domain;

import MyGame.*;

import java.util.*;

public final class RoomRoster {
    private String matchId;
    private Set<String> members = new HashSet<>();

    public RoomPrepared prepare(String roomId, PrepareRoom request) {
        if (roomId == null
                || roomId.isBlank()
                || request == null
                || !roomId.equals(request.matchId())
                || request.members() == null
                || request.members().size() != Rules.PLAYERS
                || request.members().stream().anyMatch(member -> member == null || member.isBlank())
                || new HashSet<>(request.members()).size() != request.members().size())
            throw new IllegalArgumentException("Invalid Room roster preparation");

        if (matchId != null) {
            if (!matchId.equals(request.matchId()) || !members.equals(new HashSet<>(request.members())))
                throw new IllegalStateException("A prepared Room roster cannot be replaced");
            return new RoomPrepared(matchId);
        }

        matchId = request.matchId();
        members = new HashSet<>(request.members());
        return new RoomPrepared(matchId);
    }

    public boolean canAdmit(String playerId) {
        return matchId != null && members.contains(playerId);
    }

    public RosterSnapshot capture() {
        return new RosterSnapshot(matchId, members.stream().sorted().toList());
    }

    public void restore(RosterSnapshot snapshot) {
        if (matchId != null) throw new IllegalStateException("Cannot restore a prepared Room roster");
        if (snapshot == null || snapshot.members() == null)
            throw new IllegalArgumentException("Missing Room roster snapshot");
        if (snapshot.matchId() != null)
            prepare(snapshot.matchId(), new PrepareRoom(snapshot.matchId(), snapshot.members()));
        else if (!snapshot.members().isEmpty())
            throw new IllegalArgumentException("Unprepared Room snapshot has members");
    }

    public record RosterSnapshot(String matchId, List<String> members) {}
}
