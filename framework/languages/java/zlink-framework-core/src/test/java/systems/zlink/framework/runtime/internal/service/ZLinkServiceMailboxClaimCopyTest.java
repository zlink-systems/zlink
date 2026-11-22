package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.testsupport.ChildJvm;
import systems.zlink.framework.testsupport.JfrAllocations;

import java.util.ArrayList;
import java.util.List;

/** A claim hands out the records it took from the owner queue; it does not copy them again. */
public final class ZLinkServiceMailboxClaimCopyTest {
    private static final int CLAIMS = 100;
    private static final int RECORDS_PER_CLAIM = 3;

    @Test
    void claimedRecordsKeepEnqueueOrderAndAreReadOnly() {
        try (var mailbox = new ZLinkServiceMailbox()) {
            for (int index = 0; index < RECORDS_PER_CLAIM; index++) {
                assertTrue(mailbox.tryEnqueue(record("owner", index)));
            }
            var claim =
                    mailbox.tryClaim(ZLinkServiceMailbox.Domain.APPLICATION, 8, Long.MAX_VALUE)
                            .orElseThrow();
            assertEquals(RECORDS_PER_CLAIM, claim.records().size());
            for (int index = 0; index < RECORDS_PER_CLAIM; index++) {
                assertEquals(
                        record("owner", index).correlation(),
                        claim.records().get(index).correlation());
            }
            assertThrows(UnsupportedOperationException.class, () -> claim.records().remove(0));
            assertTrue(mailbox.release(claim));
        }
    }

    @Test
    void claimingAllocatesNoSecondCopyOfTheClaimedList() throws Exception {
        ChildJvm.runInAllocationJvm(getClass());
    }

    public static void main(String[] args) throws Exception {
        try (var mailbox = new ZLinkServiceMailbox()) {
            for (int owner = 0; owner < CLAIMS; owner++) {
                for (int index = 0; index < RECORDS_PER_CLAIM; index++) {
                    mailbox.tryEnqueue(record("owner-" + owner, index));
                }
            }
            var claims = new ArrayList<ZLinkServiceMailbox.Claim>();
            var events =
                    JfrAllocations.record(
                            () -> {
                                for (int index = 0; index < CLAIMS; index++) {
                                    claims.add(
                                            mailbox.tryClaim(
                                                            ZLinkServiceMailbox.Domain.APPLICATION,
                                                            8,
                                                            Long.MAX_VALUE)
                                                    .orElseThrow());
                                }
                            });
            long claimAllocations =
                    JfrAllocations.count(
                            events,
                            event ->
                                    event.passesThrough(
                                            ZLinkServiceMailbox.class.getName(), "tryClaim"));
            long copies =
                    JfrAllocations.count(
                            events,
                            event ->
                                    event.type().startsWith("java.util.ImmutableCollections")
                                            && event.frameworkFrame()
                                                    .equals(
                                                            ZLinkServiceMailbox.class.getName()
                                                                    + ".tryClaim"));
            System.out.printf(
                    "claims=%d claimAllocations=%d immutableListCopies=%d%n",
                    CLAIMS, claimAllocations, copies);
            for (var claim : claims) assertEquals(RECORDS_PER_CLAIM, claim.records().size());
            assertTrue(claimAllocations >= CLAIMS, "claiming builds the claimed list (control)");
            assertEquals(0, copies, "the claimed list is exposed without a second copy");
        }
    }

    private static ZLinkServiceMailbox.Record record(String owner, int index) {
        return new ZLinkServiceMailbox.Record(
                owner,
                ZLinkServiceMailbox.Domain.APPLICATION,
                List.of(new byte[] {(byte) index}),
                null,
                null,
                (long) index);
    }
}
