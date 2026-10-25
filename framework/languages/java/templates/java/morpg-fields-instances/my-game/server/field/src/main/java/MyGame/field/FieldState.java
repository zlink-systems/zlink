package MyGame.field;

import MyGame.EnterField;

import java.util.*;

// This projection supports visibility; Player owns authoritative movement state.
public final class FieldState {
    public Map<String, EnterField> visiblePlayers = new LinkedHashMap<>();
}
