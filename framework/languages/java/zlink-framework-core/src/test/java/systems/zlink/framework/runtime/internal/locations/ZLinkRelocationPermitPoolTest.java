package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.locations.ZLinkLocationOptions;

final class ZLinkRelocationPermitPoolTest {
    @Test
    void acquisitionIsAtomicAcrossAllCounters() {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setMaxActiveOutboundRelocations(1);
        options.setMaxConcurrentRelocationCaptures(1);
        options.setMaxRelocationPayloadInFlightBytes(10);
        ZLinkRelocationPermitPool pool = new ZLinkRelocationPermitPool(options);

        var first = pool.tryAcquire(
            ZLinkRelocationPermitPool.Request.outbound(8, true));
        assertNotNull(first);
        assertNull(pool.tryAcquire(
            ZLinkRelocationPermitPool.Request.inbound(3, false)));
        assertEquals(1, pool.snapshot().outboundUnits());
        assertEquals(0, pool.snapshot().inboundUnits());
        assertEquals(8, pool.snapshot().payloadBytes());

        first.close();
        assertEquals(0, pool.snapshot().payloadBytes());
    }

    @Test
    void oversizedPayloadRunsAloneOnlyWhenExplicitlyAllowed() {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setMaxRelocationPayloadInFlightBytes(10);
        ZLinkRelocationPermitPool pool = new ZLinkRelocationPermitPool(options);
        var oversized = new ZLinkRelocationPermitPool.Request(
            1, 0, 0, 0, 11, true);

        var lease = pool.tryAcquire(oversized);
        assertNotNull(lease);
        assertTrue(pool.snapshot().oversizedPayloadActive());
        assertNull(pool.tryAcquire(
            ZLinkRelocationPermitPool.Request.inbound(1, false)));

        lease.close();
        assertFalse(pool.snapshot().oversizedPayloadActive());
        assertNull(pool.tryAcquire(new ZLinkRelocationPermitPool.Request(
            1, 0, 0, 0, 11, false)));
    }

    @Test
    void capturedEstimateCanOnlyShrink() {
        ZLinkRelocationPermitPool pool = new ZLinkRelocationPermitPool(
            new ZLinkLocationOptions());
        var lease = pool.tryAcquire(
            ZLinkRelocationPermitPool.Request.outbound(100, true));

        assertNotNull(lease);
        assertTrue(lease.tryShrinkPayload(40));
        assertEquals(40, lease.reservedPayloadBytes());
        assertFalse(lease.tryShrinkPayload(41));
        assertEquals(40, pool.snapshot().payloadBytes());
        lease.close();
    }

    @Test
    void asynchronousAcquisitionWaitsForTheActiveUnitToRelease()
        throws Exception {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setMaxActiveOutboundRelocations(1);
        ZLinkRelocationPermitPool pool = new ZLinkRelocationPermitPool(options);
        var request =
            ZLinkRelocationPermitPool.Request.outbound(1, false);
        var first = pool.tryAcquire(request);
        assertNotNull(first);

        var waiting = pool.acquire(request, () -> false)
            .toCompletableFuture();
        assertFalse(waiting.isDone());

        first.close();
        var second = waiting.get(1, TimeUnit.SECONDS);
        assertNotNull(second);
        assertEquals(1, pool.snapshot().outboundUnits());
        second.close();
    }
}
