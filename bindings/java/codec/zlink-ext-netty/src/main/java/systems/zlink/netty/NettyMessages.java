package systems.zlink.netty;

import systems.zlink.contracts.messaging.Message;
import io.netty.buffer.ByteBuf;

public final class NettyMessages {
    private NettyMessages() {}

    public static Message from(ByteBuf source) {
        return NettyMessageAdapter.from(source);
    }

    public static int copyTo(Message message, ByteBuf destination) {
        return NettyMessageAdapter.copyTo(message, destination);
    }
}
