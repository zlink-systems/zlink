package systems.zlink.stream.connector;

/**
 * Flow metadata attached to an inbound stream message.
 */
public interface ZLinkStreamFlow {
    String flowId();

    ZLinkFlowOrigin flowOrigin();
}
