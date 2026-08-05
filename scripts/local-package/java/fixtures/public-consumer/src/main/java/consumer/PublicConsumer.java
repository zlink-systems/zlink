package consumer;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorStatus;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.StreamSocket;

final class PublicConsumer {
    public static void main(String[] args) {
        int[] version = Zlink.version();
        if (version.length != 3 || version[0] != @CORE_MAJOR@
                || version[1] != @CORE_MINOR@ || version[2] != @CORE_PATCH@) {
            throw new IllegalStateException(
                "Expected packaged Core @CORE_VERSION@, found "
                    + java.util.Arrays.toString(version));
        }
        System.out.println("ZLINK_CORE_VERSION=@CORE_VERSION@");
    }

    static void verify(Context context, Socket socket, StreamSocket stream,
                       MonitorStatus status, SendOperation send) {
        try (var monitor = socket.monitorOpen()) {
            status.isReady();
        }
        stream.onPacket((routingId, header, body) -> {
            header.close();
            body.close();
        });
        try (Message first = Message.from("first");
             Message second = Message.from("second")) {
            send.message(first).message(second);
        }
        context.shutdown();
    }

    private PublicConsumer() {
    }
}
