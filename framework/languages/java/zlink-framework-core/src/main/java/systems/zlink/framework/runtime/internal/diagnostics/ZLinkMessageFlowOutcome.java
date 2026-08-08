package systems.zlink.framework.runtime.internal.diagnostics;

// A transition or dispatch error outcome in a message's lifecycle.
// RECEIVED/DISPATCHED/REPLIED are inbound (this node receives);
// SENT/REPLY_RECEIVED are outbound (this node sends).
public enum ZLinkMessageFlowOutcome {
    RECEIVED(0),
    DISPATCHED(1),
    REPLIED(2),
    DROPPED(3),
    SENT(4),
    REPLY_RECEIVED(5),
    ERROR(6);

    private final int value;

    ZLinkMessageFlowOutcome(int value) { this.value = value; }

    public int value() { return value; }
}
