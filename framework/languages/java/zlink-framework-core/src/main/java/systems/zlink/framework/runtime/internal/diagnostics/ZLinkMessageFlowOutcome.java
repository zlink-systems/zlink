package systems.zlink.framework.runtime.internal.diagnostics;

// A message-flow phase. Dispatch errors use a distinct event id and have no phase.
public enum ZLinkMessageFlowOutcome {
    RECEIVED(0),
    ADMITTED(1),
    DISPATCHED(2),
    COMPLETED(3),
    REPLIED(4),
    SENT(5),
    REPLY_RECEIVED(6),
    BACKPRESSURED(7),
    DROPPED(8);

    private final int value;

    ZLinkMessageFlowOutcome(int value) { this.value = value; }

    public int value() { return value; }
}
