package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.Optional;

final class ZLinkInstanceActivationComparisonTest {
    private final ZLinkInstanceActivationRecoveryCodec codec =
            new ZLinkInstanceActivationRecoveryCodec();

    @Test
    void matchingKind2RouteAndEnvelopePassComparison() {
        codec.validateColdActivation(message(), envelope("match"), new byte[] {1});
    }

    @ParameterizedTest
    @ValueSource(
            strings = {
                "mesh",
                "type",
                "descriptor",
                "deadline",
                "operationHigh",
                "operationLow",
                "metadataPresence",
                "metadataBytes",
                "spot",
                "target",
                "targetGeneration",
                "source",
                "sourceGeneration",
                "sourceSpot",
                "request",
                "reply"
            })
    void eachKind2RouteEnvelopeMismatchIsProtocolError(String field) {
        var failure =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                codec.validateColdActivation(
                                        message(), envelope(field), new byte[] {1}));
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
    }

    private static ZLinkServiceM6BWireCodec.InstanceSpotMessage message() {
        return new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                ServiceWireConstants.FLAG_METADATA,
                new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                        RoutingId.from("target"), 1, "spot", "mesh", "type", "version", 100),
                true,
                2,
                RoutingId.from("source"),
                "source-spot",
                true,
                3,
                4,
                5L);
    }

    private static ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope(String field) {
        return new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                field.equals("spot") ? "different" : "spot",
                field.equals("type") ? "different" : "type",
                field.equals("mesh") ? "different" : "mesh",
                RoutingId.from(field.equals("target") ? "different" : "target"),
                field.equals("targetGeneration") ? 2 : 1,
                field.equals("descriptor") ? "different" : "version",
                RoutingId.from(field.equals("source") ? "different" : "source"),
                field.equals("sourceGeneration") ? 1 : 2,
                Optional.of(field.equals("sourceSpot") ? "different" : "source-spot"),
                !field.equals("request"),
                field.equals("operationHigh") ? 4 : 3,
                field.equals("operationLow") ? 3 : 4,
                field.equals("reply") ? 6L : 5L,
                field.equals("deadline") ? 101 : 100,
                field.equals("metadataPresence")
                        ? new byte[0]
                        : new byte[] {(byte) (field.equals("metadataBytes") ? 2 : 1)},
                new byte[] {1});
    }
}
