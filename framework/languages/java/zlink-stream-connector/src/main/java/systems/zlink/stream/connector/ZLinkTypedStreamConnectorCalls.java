package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletionStage;

record ZLinkTypedStreamConnectorSendCall(
    ZLinkStreamSendCall delegate) implements ZLinkTypedStreamSendCall {
    @Override
    public ZLinkTypedStreamSendCall packetName(String name) {
        return new ZLinkTypedStreamConnectorSendCall(delegate.packetName(name));
    }

    @Override
    public ZLinkTypedStreamSendCall metadata(String key, String value) {
        return new ZLinkTypedStreamConnectorSendCall(delegate.metadata(key, value));
    }

    @Override
    public ZLinkTypedStreamSendCall metadata(Map<String, String> metadata) {
        return new ZLinkTypedStreamConnectorSendCall(delegate.metadata(metadata));
    }

    @Override
    public ZLinkTypedStreamSendCall compress() {
        return new ZLinkTypedStreamConnectorSendCall(delegate.compress());
    }

    @Override
    public CompletionStage<Void> submit() {
        return delegate.submit();
    }
}

record ZLinkTypedStreamConnectorRequestCall(
    ZLinkStreamRequestCall delegate) implements ZLinkTypedStreamRequestCall {
    @Override
    public ZLinkTypedStreamRequestCall packetName(String name) {
        return new ZLinkTypedStreamConnectorRequestCall(delegate.packetName(name));
    }

    @Override
    public ZLinkTypedStreamRequestCall metadata(String key, String value) {
        return new ZLinkTypedStreamConnectorRequestCall(delegate.metadata(key, value));
    }

    @Override
    public ZLinkTypedStreamRequestCall metadata(Map<String, String> metadata) {
        return new ZLinkTypedStreamConnectorRequestCall(delegate.metadata(metadata));
    }

    @Override
    public ZLinkTypedStreamRequestCall timeout(Duration timeout) {
        return new ZLinkTypedStreamConnectorRequestCall(delegate.timeout(timeout));
    }

    @Override
    public ZLinkTypedStreamRequestCall compress() {
        return new ZLinkTypedStreamConnectorRequestCall(delegate.compress());
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        return delegate.submit(replyType);
    }
}
