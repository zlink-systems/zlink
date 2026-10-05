package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

import java.util.List;
import java.util.concurrent.CompletionStage;

final class ZLinkProviderCreationDescriptorTest {
    @ParameterizedTest
    @CsvSource({
        "Reserve, ACTOR",
        "Commit, ACTOR",
        "Abort, ACTOR",
        "Reserve, USER_SPOT",
        "Commit, USER_SPOT",
        "Abort, USER_SPOT"
    })
    void creationTransitionsFenceOwnerLeaseAndDescriptorVersion(
            String transition, ZLinkPlacementObjectKind objectKind) {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture(objectKind);
        if (transition.equals("Reserve")) {
            assertEquals(
                    ZLinkObjectAbortResult.ABORTED,
                    await(fixture.repository.abort(fixture.reservation, () -> false)));
        }
        fixture.provider.writes.clear();
        var descriptorKey = descriptorKey(fixture);
        var descriptor =
                (ZLinkStoreReadFound) await(fixture.provider.read(descriptorKey, () -> false));
        apply(fixture, transition);
        var conditions = fixture.provider.writes.getFirst().conditions();
        assertEquals(transition.equals("Reserve") ? 6 : 4, conditions.size());
        assertTrue(
                conditions.stream()
                        .anyMatch(
                                c ->
                                        c instanceof ZLinkStoreValueCondition v
                                                && v.key()
                                                        .equals(
                                                                ZLinkOpaqueRecordKey.of(
                                                                        "owner-lease",
                                                                        fixture.owner.ownerId()))),
                "owner lease Value");
        assertTrue(
                conditions.stream()
                        .anyMatch(
                                c ->
                                        c instanceof ZLinkStoreVersionCondition v
                                                && v.key().equals(descriptorKey)
                                                && v.expected()
                                                        .equals(descriptor.value().version())),
                "descriptor StoreVersion");
    }

    @ParameterizedTest
    @CsvSource({
        "Reserve, ACTOR",
        "Commit, ACTOR",
        "Abort, ACTOR",
        "Reserve, USER_SPOT",
        "Commit, USER_SPOT",
        "Abort, USER_SPOT"
    })
    void sameLifecycleDescriptorRepublishRebuildsQualifiedConflict(
            String transition, ZLinkPlacementObjectKind objectKind) {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture(objectKind);
        if (transition.equals("Reserve")) {
            await(fixture.repository.abort(fixture.reservation, () -> false));
        }
        fixture.provider.writes.clear();
        fixture.provider.beforeNextWrite = ignored -> republish(fixture, null, null);
        apply(fixture, transition);
        assertEquals(2, fixture.provider.writes.size(), "CAS conflict followed by rebuilt write");
    }

    @Test
    void previousLifecycleCommitIsRejected() {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture();
        fixture.provider.beforeNextWrite = ignored -> republish(fixture, 9L, null);
        assertEquals(
                ZLinkObjectCommitResult.STALE,
                await(fixture.repository.commit(fixture.reservation, new byte[] {9}, () -> false)));
        assertEquals(1, fixture.provider.writes.size());
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        await(
                                fixture.repository.read(
                                        fixture.reservation.authorityKey(), () -> false)));
        assertTrue(authority.pendingCreation().isPresent());
    }

    @Test
    void acceptedCreationCompletesWhileTargetDraining() {
        var fixture = new ZLinkProviderCreationTerminalTest.Fixture();
        republish(fixture, null, "draining");
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                fixture.complete(fixture.terminal(ZLinkCreationTerminalState.CREATED)));
    }

    private static void apply(
            ZLinkProviderCreationTerminalTest.Fixture fixture, String transition) {
        switch (transition) {
            case "Reserve" ->
                    assertInstanceOf(
                            ZLinkObjectReserved.class,
                            await(
                                    fixture.repository.reserve(
                                            fixture.request("actor"), () -> false)));
            case "Commit" ->
                    assertEquals(
                            ZLinkObjectCommitResult.COMMITTED,
                            await(
                                    fixture.repository.commit(
                                            fixture.reservation, new byte[] {9}, () -> false)));
            case "Abort" ->
                    assertEquals(
                            ZLinkObjectAbortResult.ABORTED,
                            await(fixture.repository.abort(fixture.reservation, () -> false)));
            default -> throw new AssertionError(transition);
        }
    }

    private static ZLinkStoreKey descriptorKey(ZLinkProviderCreationTerminalTest.Fixture fixture) {
        return ZLinkOpaqueRecordKey.of(
                "mesh-node", fixture.descriptor.meshName(), fixture.descriptor.rid().toHex());
    }

    private static void republish(
            ZLinkProviderCreationTerminalTest.Fixture fixture, Long lifecycle, String state) {
        var key = descriptorKey(fixture);
        var read = (ZLinkStoreReadFound) await(fixture.provider.delegate.read(key, () -> false));
        try {
            var mapper = new ObjectMapper();
            var record = (ObjectNode) mapper.readTree(read.value().bytes());
            var descriptor = (ObjectNode) record.get("descriptor");
            if (lifecycle != null) descriptor.put("lifecycleGeneration", lifecycle);
            if (state != null) descriptor.put("state", state);
            await(
                    fixture.provider.delegate.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(
                                                    key, mapper.writeValueAsBytes(record), null))),
                            () -> false));
        } catch (java.io.IOException error) {
            throw new AssertionError(error);
        }
    }

    private static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().join();
    }
}
