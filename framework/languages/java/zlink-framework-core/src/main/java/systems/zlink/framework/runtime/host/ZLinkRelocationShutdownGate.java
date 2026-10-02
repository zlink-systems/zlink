package systems.zlink.framework.runtime.host;

/**
 * Orders a host Shutdown request against the currently executing relocation unit. Shutdown may stop
 * later units, but it cannot run teardown concurrently with the unit that already crossed its
 * source barrier.
 */
final class ZLinkRelocationShutdownGate {
    private boolean relocationUnitInProgress;
    private final java.util.concurrent.CompletableFuture<Void> shutdownRequested =
            new java.util.concurrent.CompletableFuture<>();

    synchronized boolean beginRelocationUnit() {
        if (shutdownRequested.isDone()) {
            return false;
        }
        relocationUnitInProgress = true;
        return true;
    }

    boolean requestShutdown() {
        shutdownRequested.complete(null);
        synchronized (this) {
            return !relocationUnitInProgress;
        }
    }

    java.util.concurrent.CompletionStage<Void> stopSignal() {
        return shutdownRequested;
    }

    synchronized void finishRelocationUnit() {
        relocationUnitInProgress = false;
    }

    synchronized boolean stopBeforeNextUnit() {
        return shutdownRequested.isDone();
    }
}
