package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.URI;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ScriptedStreamTransport implements ZLinkStreamTransportConnection {
    private final EndingGate closeGate;
    private final CompletableFuture<ZLinkStreamWireProtocol.Header> written =
            new CompletableFuture<>();
    private CompletableFuture<ZLinkStreamWireProtocol.Frame> inbound = new CompletableFuture<>();
    private volatile boolean open = true;

    ScriptedStreamTransport(EndingGate closeGate) {
        this.closeGate = closeGate;
    }

    static ZLinkStreamConnectorOptions options() {
        return new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:1"),
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(30),
                1,
                Duration.ofSeconds(5),
                64 * 1024,
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                false,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0);
    }

    CompletionStage<ZLinkStreamWireProtocol.Header> written() {
        return written;
    }

    @Override
    public CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync() {
        return inbound;
    }

    @Override
    public CompletionStage<Void> writeAsync(byte[] frame) {
        written.complete(
                ZLinkStreamWireProtocol.decodeHeader(
                        ZLinkStreamWireProtocol.decodeFrame(frame).header()));
        return CompletableFuture.completedFuture(null);
    }

    void deliverInvalidHeader() {
        deliver(new ZLinkStreamWireProtocol.Frame(new byte[] {-1, -1, -1, -1}, new byte[0]));
    }

    void deliverServerDrain() {
        byte[] header =
                ZLinkStreamWireProtocol.encodeHeader(
                        new ZLinkStreamWireProtocol.Header(
                                ZLinkStreamWireProtocol.KIND_CONTROL,
                                ZLinkStreamWireProtocol.CODEC_RAW,
                                0,
                                null,
                                ZLinkSessionClosingControl.NAME,
                                Map.of(),
                                null));
        deliver(new ZLinkStreamWireProtocol.Frame(header, new byte[] {1, 4, 0, 0}));
    }

    void reply(long requestSeq) {
        byte[] header =
                ZLinkStreamWireProtocol.encodeHeader(
                        new ZLinkStreamWireProtocol.Header(
                                ZLinkStreamWireProtocol.KIND_RESPONSE,
                                ZLinkStreamWireProtocol.CODEC_RAW,
                                ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
                                requestSeq,
                                "",
                                Map.of(),
                                null));
        deliver(new ZLinkStreamWireProtocol.Frame(header, new byte[] {7}));
    }

    private void deliver(ZLinkStreamWireProtocol.Frame frame) {
        var read = inbound;
        inbound = new CompletableFuture<>();
        assertTrue(read.complete(frame));
    }

    @Override
    public boolean isOpen() {
        return open;
    }

    @Override
    public void close() {
        if (closeGate != null) {
            closeGate.hold();
        }
        open = false;
    }

    static final class EndingGate {
        private final CompletableFuture<Void> entered = new CompletableFuture<>();
        private final CompletableFuture<Void> release = new CompletableFuture<>();

        void awaitEntered(long timeoutSeconds) throws Exception {
            entered.get(timeoutSeconds, TimeUnit.SECONDS);
        }

        void release() {
            release.complete(null);
        }

        void hold() {
            entered.complete(null);
            release.join();
        }
    }
}
