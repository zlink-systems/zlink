package systems.zlink.framework.testsupport;

import static org.junit.jupiter.api.Assertions.assertEquals;

import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.io.DataInputStream;
import java.io.IOException;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.util.concurrent.TimeUnit;

public final class ZLinkStreamTestFrames {
    private ZLinkStreamTestFrames() {}

    public static Frame readSkippingHeartbeatPings(Socket socket) throws IOException {
        var input = new DataInputStream(socket.getInputStream());
        int timeout = socket.getSoTimeout();
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeout);
        try {
            while (true) {
                if (timeout > 0) {
                    long remaining = deadline - System.nanoTime();
                    if (remaining <= 0) {
                        throw new SocketTimeoutException("Timed out waiting for a STREAM frame");
                    }
                    socket.setSoTimeout((int) Math.ceilDiv(remaining, 1_000_000L));
                }
                int headerSize = input.readUnsignedShort();
                int bodySize = input.readInt();
                byte[] headerBytes = new byte[headerSize];
                byte[] body = new byte[bodySize];
                input.readFully(headerBytes);
                input.readFully(body);
                var header = ZLinkStreamHeaderCodec.decodeOrPlain(headerBytes);
                if (header.kind() != ZLinkStreamMessageKind.CONTROL
                        || !ZLinkStreamHeader.HEARTBEAT_PING.name().equals(header.name())) {
                    return new Frame(header, headerBytes, body);
                }
                assertEquals(0, body.length);
                socket.getOutputStream()
                        .write(
                                ZLinkStreamFrameCodec.encode(
                                        ZLinkStreamHeader.HEARTBEAT_PONG, body));
            }
        } finally {
            socket.setSoTimeout(timeout);
        }
    }

    public record Frame(ZLinkStreamHeader header, byte[] headerBytes, byte[] body) {}
}
