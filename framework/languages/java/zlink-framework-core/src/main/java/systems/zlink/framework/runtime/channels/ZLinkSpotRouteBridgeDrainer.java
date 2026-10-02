package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

import java.util.Map;

/** Drains bridge records on their router receive owner's readable turn. */
final class ZLinkSpotRouteBridgeDrainer {
    private final Map<String, ZLinkBackendSpotRouteBridge> bridges;
    private volatile Runnable dispatchDrainer;

    ZLinkSpotRouteBridgeDrainer(Map<String, ZLinkBackendSpotRouteBridge> bridges) {
        this.bridges = bridges;
    }

    void setDispatchDrainer(Runnable dispatchDrainer) {
        this.dispatchDrainer = dispatchDrainer;
    }

    void drainNow(String channelName) {
        ZLinkBackendSpotRouteBridge bridge = bridges.get(channelName);
        if (bridge == null) return;
        try {
            bridge.drain();
        } catch (RuntimeException failure) {
            if (!ZLinkChannelRuntime.isNoDataReceive(failure)) throw failure;
        }
        Runnable drainer = dispatchDrainer;
        if (drainer != null) drainer.run();
    }
}
