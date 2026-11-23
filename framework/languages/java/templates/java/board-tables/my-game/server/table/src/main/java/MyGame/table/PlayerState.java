package MyGame.table;

import MyGame.MatchAssigned;

import java.util.HashSet;
import java.util.Set;

public final class PlayerState {
    public String ticketId;
    public MatchAssigned assignment;
    public String joining;
    public String withdrawnRoom;
    public Set<String> withdrawnAssignments = new HashSet<>();
    public boolean disconnected;
}
