package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceRouteFence;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkInstanceAdmissionFenceTest {
    private static final InstanceRouteFence CURRENT =
            route("node", Long.MIN_VALUE, 4, "owner", 7, 2);

    private static InstanceRouteFence route(
            String node, long lifecycle, long object, String owner, long authority, long lease) {
        return new InstanceRouteFence(
                RoutingId.from(node),
                lifecycle,
                "room",
                object,
                owner,
                authority,
                lease,
                "store-version");
    }

    @Test
    void acceptsCurrentAndEarlierIncarnationOfSameOwner() {
        assertNull(ZLinkSpotRuntime.instanceApplicationAdmissionFailure(CURRENT, CURRENT));
        assertNull(
                ZLinkSpotRuntime.instanceApplicationAdmissionFailure(
                        CURRENT, route("node", Long.MIN_VALUE, 3, "owner", 6, 2)));
    }

    @Test
    void refusesDifferentOwnerLeaseNodeAndLifecycleAcrossIncarnations() {
        for (var route :
                new InstanceRouteFence[] {
                    route("node", Long.MIN_VALUE, 3, "other", 6, 2),
                    route("node", Long.MIN_VALUE, 3, "owner", 6, 1),
                    route("other-node", Long.MIN_VALUE, 3, "owner", 6, 2),
                    route("node", 1, 3, "owner", 6, 2)
                }) {
            assertMoving(route);
        }
    }

    @Test
    void refusesOwnerOrAuthorityChangeWithinSameIncarnation() {
        assertMoving(route("node", Long.MIN_VALUE, 4, "other", 7, 2));
        assertMoving(route("node", Long.MIN_VALUE, 4, "owner", 6, 2));
    }

    @Test
    void refusesCurrentIncarnationBehindRequestedGenerationWithTypedResult() {
        var rejection =
                assertInstanceOf(
                        ZLinkFrameworkException.class,
                        ZLinkSpotRuntime.instanceApplicationAdmissionFailure(
                                CURRENT, route("node", Long.MIN_VALUE, 5, "owner", 8, 2)));
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, rejection.kind());
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_SPOT_GENERATION_STALE,
                ZLinkRequestFailureMapping.causeCode(rejection));
    }

    private static void assertMoving(InstanceRouteFence requested) {
        var rejection =
                assertInstanceOf(
                        ZLinkFrameworkException.class,
                        ZLinkSpotRuntime.instanceApplicationAdmissionFailure(CURRENT, requested));
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, rejection.kind());
        assertEquals(
                ServiceWireConstants.FRAMEWORK_ERROR_SPOT_MOVING,
                ZLinkRequestFailureMapping.causeCode(rejection));
    }
}
