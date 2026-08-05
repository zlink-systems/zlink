package systems.zlink.framework.configuration;

// Resolve at runtime to turn message-flow tracing on/off (or change verbosity)
// without a restart. The change is read live by every dispatch surface. Thread-safe.
public interface ZLinkMessageFlowControl {
    void setMessageFlowMode(ZLinkMessageFlowLogMode mode);

    ZLinkMessageFlowLogMode messageFlowMode();
}
