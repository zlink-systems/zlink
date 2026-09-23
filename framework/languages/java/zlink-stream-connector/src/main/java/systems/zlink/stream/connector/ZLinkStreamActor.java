package systems.zlink.stream.connector;

public interface ZLinkStreamActor {
    String actorId();

    boolean isBound();

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);

    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);

    ZLinkTypedStreamSendCall send(Object payload);

    ZLinkTypedStreamRequestCall request(Object payload);

    AutoCloseable on(String name, ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler);

    <TPayload> AutoCloseable on(
            Class<TPayload> payloadType, ZLinkStreamMessageHandler<TPayload> handler);

    <TPayload> AutoCloseable on(
            String name, Class<TPayload> payloadType, ZLinkStreamMessageHandler<TPayload> handler);
}
