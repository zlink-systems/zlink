package systems.zlink.framework.runtime.host;

/**
 * Orders a host Shutdown request against the currently executing relocation
 * unit. Shutdown may stop later units, but it cannot run teardown concurrently
 * with the unit that already crossed its source barrier.
 */
final class ZLinkRelocationShutdownGate {
    private boolean relocationUnitInProgress;
    private boolean shutdownRequested;

    synchronized boolean beginRelocationUnit() {
        if (shutdownRequested) {
            return false;
        }
        relocationUnitInProgress = true;
        return true;
    }

    synchronized boolean requestShutdown() {
        shutdownRequested = true;
        return !relocationUnitInProgress;
    }

    synchronized void finishRelocationUnit() {
        relocationUnitInProgress = false;
    }

    synchronized boolean stopBeforeNextUnit() {
        return shutdownRequested;
    }
}
