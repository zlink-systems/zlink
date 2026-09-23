package systems.zlink.stream.connector;

@FunctionalInterface
public interface ZLinkStreamRequestSendingHandler {
    void handle(ZLinkStreamRequestSendingContext context);
}
