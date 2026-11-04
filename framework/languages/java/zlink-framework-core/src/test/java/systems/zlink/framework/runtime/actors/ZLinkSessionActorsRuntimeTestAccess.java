package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import java.time.Duration;
import java.util.Collection;

/** Test-only access to Session Actor owner state. */
public final class ZLinkSessionActorsRuntimeTestAccess {
    private ZLinkSessionActorsRuntimeTestAccess() {}

    public static Duration sessionRelocationSealTimeout(ZLinkSessionActorsRuntime runtime) {
        return runtime.sessionRelocationSealTimeout;
    }

    public static ZLinkInternalSpotNode spotNode(ZLinkSessionActorsRuntime runtime) {
        return runtime.spotNode;
    }

    public static Collection<?> sealTerminals(ZLinkSessionActorsRuntime runtime) {
        return runtime.sealTerminals.values();
    }

    public static void expireFirstRelocationSeal(ZLinkSessionActorsRuntime runtime) {
        runtime.expireRelocationSeal(runtime.sealTerminals.values().iterator().next());
    }
}
