package systems.zlink.framework.runtime.internal.diagnostics;

public enum ZLinkActivationState {
    ACTIVATING("activating"),
    READY("ready"),
    CLOSING("closing");

    private final String traceName;

    ZLinkActivationState(String traceName) {
        this.traceName = traceName;
    }

    public String traceName() {
        return traceName;
    }
}
