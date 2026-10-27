package MyGame;

import com.fasterxml.jackson.annotation.JsonProperty;

public enum PlayerLeftReason {
    @JsonProperty("left-zone") LEFT_ZONE,
    @JsonProperty("disconnected") DISCONNECTED,
    @JsonProperty("out-of-range") OUT_OF_RANGE
}
