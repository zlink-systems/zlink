package MyGame;

import com.fasterxml.jackson.annotation.JsonProperty;

public enum BorderPresenceKind {
    @JsonProperty("appeared") APPEARED,
    @JsonProperty("moved") MOVED,
    @JsonProperty("left") LEFT
}
