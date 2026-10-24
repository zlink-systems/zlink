package MyGame.zone;

import MyGame.EnterZone;

import java.util.*;

// This projection supports visibility; Player owns authoritative movement state.
public final class ZoneState {
    public Map<String, EnterZone> visiblePlayers = new LinkedHashMap<>();
    public Map<String, EnterZone> arrivals = new LinkedHashMap<>();
}
