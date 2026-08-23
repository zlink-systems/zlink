package systems.zlink.crosslanguage.host;

/**
 * The host's own routing id as a real Spring bean.
 *
 * Handlers activated by the Framework are constructed by the Framework's own
 * activator, and only proper bean definitions resolve there -- injecting the
 * manually registered {@code hostArgs} singleton into a handler constructor
 * makes the activation fail, which surfaces as a silent NOT_FOUND-style error
 * reply with no handler invocation at all.
 */
public final class NodeIdentity {
    private final String nodeRid;

    public NodeIdentity(String nodeRid) {
        this.nodeRid = nodeRid;
    }

    public String nodeRid() {
        return nodeRid;
    }
}
