package systems.zlink.framework.testsupport;

import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

/**
 * Waits for a host's own Serving status. A fixed-RID host finishes startup in Preparing and serves
 * only after 3·margin (MeshNode §3.3), so callers wait for that status before sending work.
 */
public final class ZLinkHostServing {
    private ZLinkHostServing() {}

    public static void await(ZLinkFrameworkRuntime runtime) throws Exception {
        var ready =
                ZLinkStatusAwait.matching(runtime.observe(), ZLinkFrameworkRuntimeStatus::isReady);
        try {
            ready.get();
        } finally {
            ready.cancel(false);
        }
    }
}
