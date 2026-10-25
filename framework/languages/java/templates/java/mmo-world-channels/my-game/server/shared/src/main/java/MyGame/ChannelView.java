package MyGame;

import java.util.List;

public record ChannelView(long channelId, List<String> playerIds) {
    public ChannelView {
        playerIds = List.copyOf(playerIds);
    }
}
