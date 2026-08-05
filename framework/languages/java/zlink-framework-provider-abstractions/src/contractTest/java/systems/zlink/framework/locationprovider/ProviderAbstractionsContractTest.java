package systems.zlink.framework.locationprovider;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.Arrays;
import java.util.HashMap;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;
import org.junit.jupiter.api.Test;

final class ProviderAbstractionsContractTest {
    @Test
    void locationStoreExposesOnlyOpaquePrimitives() {
        Set<String> methods = Arrays.stream(ZLinkLocationStore.class.getMethods())
            .map(method -> method.getName())
            .collect(Collectors.toSet());

        assertEquals(Set.of("read", "write", "scan"), methods);
        assertTrue(Arrays.stream(ZLinkLocationStore.class.getMethods())
            .flatMap(method -> Arrays.stream(method.getParameterTypes()))
            .allMatch(type -> type.getPackageName().equals(
                "systems.zlink.framework.locationprovider")));
    }

    @Test
    void relocationPutRequiresCallerIssuedReference() throws Exception {
        assertEquals(
            ZLinkBlobReference.class,
            ZLinkRelocationStore.class
                .getMethod(
                    "put",
                    ZLinkBlobReference.class,
                    byte[].class,
                    java.time.Duration.class,
                    ZLinkStoreCancellation.class)
                .getParameterTypes()[0]);
    }

    @Test
    void bytePayloadsDoNotExposeMutableProviderStorage() {
        byte[] source = new byte[] {1, 2, 3};
        var value = new ZLinkStoreValue(
            source,
            new ZLinkStoreVersion("1"),
            null,
            Instant.EPOCH);
        source[0] = 9;
        byte[] returned = value.bytes();
        returned[1] = 9;

        assertArrayEquals(new byte[] {1, 2, 3}, value.bytes());
    }

    @Test
    void providerCollectionsAreImmutableSnapshots() {
        var conditions = new ArrayList<ZLinkStoreCondition>();
        conditions.add(new ZLinkStoreMissingCondition(
            new ZLinkStoreKey("a")));
        var request = new ZLinkStoreWriteRequest(
            conditions,
            List.of());
        conditions.clear();

        var versions = new HashMap<ZLinkStoreKey, ZLinkStoreVersion>();
        versions.put(
            new ZLinkStoreKey("a"),
            new ZLinkStoreVersion("1"));
        var applied = new ZLinkStoreWriteApplied(
            versions,
            Instant.EPOCH);
        versions.clear();

        assertEquals(1, request.conditions().size());
        assertEquals(1, applied.putVersions().size());
        assertThrowsUnsupported(
            () -> request.conditions().clear());
        assertThrowsUnsupported(
            () -> applied.putVersions().clear());
    }

    private static void assertThrowsUnsupported(Runnable action) {
        org.junit.jupiter.api.Assertions.assertThrows(
            UnsupportedOperationException.class,
            action::run);
    }
}
