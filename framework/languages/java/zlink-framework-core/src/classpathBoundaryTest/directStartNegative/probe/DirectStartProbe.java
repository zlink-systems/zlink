package probe;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

public final class DirectStartProbe {
    public void start() {
        ZLinkFrameworkRuntime.start(null, null);
    }
}
