package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

final class ZLinkRelocationShutdownGateTest {
    @Test
    void stopReceiptPublishesOutsideUnitOwnerLockAndRejectsLaterClaims() throws Exception {
        var gate = new ZLinkRelocationShutdownGate();
        var lockHeld = new java.util.concurrent.atomic.AtomicBoolean();
        var observed = new java.util.concurrent.CompletableFuture<Void>();
        gate.stopSignal()
                .thenRun(
                        () -> {
                            lockHeld.set(Thread.holdsLock(gate));
                            observed.complete(null);
                        });
        java.util.concurrent.CompletableFuture<Boolean> request;
        synchronized (gate) {
            request = java.util.concurrent.CompletableFuture.supplyAsync(gate::requestShutdown);
            observed.get(3, java.util.concurrent.TimeUnit.SECONDS);
            assertFalse(gate.beginRelocationUnit());
        }
        assertTrue(request.get(3, java.util.concurrent.TimeUnit.SECONDS));
        assertFalse(lockHeld.get());
    }

    @Test
    void shutdownWaitsForCurrentUnitAndStopsTheNextUnit() {
        ZLinkRelocationShutdownGate gate = new ZLinkRelocationShutdownGate();

        assertTrue(gate.beginRelocationUnit());
        assertFalse(gate.requestShutdown());
        assertTrue(gate.stopBeforeNextUnit());

        gate.finishRelocationUnit();
        assertFalse(gate.beginRelocationUnit());
    }

    @Test
    void shutdownWithoutActiveRelocationCanStartDrainImmediately() {
        ZLinkRelocationShutdownGate gate = new ZLinkRelocationShutdownGate();

        assertTrue(gate.requestShutdown());
        assertTrue(gate.stopBeforeNextUnit());
        assertFalse(gate.beginRelocationUnit());
    }
}
