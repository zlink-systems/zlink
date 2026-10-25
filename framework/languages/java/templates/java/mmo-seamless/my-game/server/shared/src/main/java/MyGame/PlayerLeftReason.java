package MyGame;

import com.fasterxml.jackson.annotation.JsonProperty;

public enum PlayerLeftReason {
    @JsonProperty("left-zone") LEFT_ZONE,
    @JsonProperty("disconnected") DISCONNECTED
}
