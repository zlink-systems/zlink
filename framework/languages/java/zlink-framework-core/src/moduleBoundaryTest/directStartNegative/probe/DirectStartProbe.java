package probe;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

final class DirectStartProbe {
    void start() {
        ZLinkFrameworkRuntime.start(null, null);
    }
}
