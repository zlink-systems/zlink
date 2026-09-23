package systems.zlink.stream.connector;

@FunctionalInterface
public interface ZLinkStreamReplyReceivedHandler {
    void handle(ZLinkStreamReplyReceivedContext context);
}
