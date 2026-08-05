package systems.zlink.netty;

import systems.zlink.contracts.messaging.Message;
import io.netty.buffer.ByteBuf;
import java.util.Objects;

final class NettyMessageAdapter {

    private NettyMessageAdapter() {}

    /** Copies the readable bytes from the {@code ByteBuf} without advancing it. */
    public static Message from(ByteBuf source) {
        Objects.requireNonNull(source, "source");
        return Message.from(source);
    }

    /** Copies the message data into the writable region of {@code destination}. */
    public static int copyTo(Message message, ByteBuf destination) {
        Objects.requireNonNull(message, "message");
        Objects.requireNonNull(destination, "destination");
        int size = message.size();
        if (destination.writableBytes() < size)
            throw new IllegalArgumentException("destination buffer too small");
        return message.copyTo(destination);
    }
}
