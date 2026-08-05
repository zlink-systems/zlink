package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

final class ZLinkRelocationShutdownGateTest {
    @Test
    void shutdownWaitsForCurrentUnitAndStopsTheNextUnit() {
        ZLinkRelocationShutdownGate gate =
            new ZLinkRelocationShutdownGate();

        assertTrue(gate.beginRelocationUnit());
        assertFalse(gate.requestShutdown());
        assertTrue(gate.stopBeforeNextUnit());

        gate.finishRelocationUnit();
        assertFalse(gate.beginRelocationUnit());
    }

    @Test
    void shutdownWithoutActiveRelocationCanStartDrainImmediately() {
        ZLinkRelocationShutdownGate gate =
            new ZLinkRelocationShutdownGate();

        assertTrue(gate.requestShutdown());
        assertTrue(gate.stopBeforeNextUnit());
        assertFalse(gate.beginRelocationUnit());
    }
}
