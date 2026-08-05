package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

interface ZLinkStreamTransportConnection extends AutoCloseable {
    CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync();

    CompletionStage<Void> writeAsync(byte[] frame);

    boolean isOpen();

    @Override
    void close();
}
