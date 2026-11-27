package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Connection;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ConnectionRef;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Delivery;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;

import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkAdmissionQueueConsumedTicketTest {
    private static final String NODE = AdmissionQueueFixture.id("node");
    private static final long EPOCH = 42L;

    @Test
    void usedTicketLeavesTheQueueAndBudgetButCanStillAdmitUntilExpiry() {
        var clock = new AtomicLong(10_000);
        var queue =
                new ZLinkAdmissionQueueState(
                        new ZLinkAdmissionTickets(AdmissionQueueFixture.KEY, null),
                        1,
                        clock::get,
                        EPOCH);
        var first = connection("first");
        var second = connection("second");
        var third = connection("third");
        queue.seedNode(NODE, 1, 3, 0, 0);
        assertEquals(Delivery.TICKET, queue.enqueue(first).delivery().kind());
        assertEquals(1, queue.enqueue(second).delivery().position());
        assertEquals(2, queue.enqueue(third).delivery().position());
        var firstPermit = queue.admitPermit(permit(first, 1));
        assertTrue(firstPermit.granted());
        queue.releasePermit(firstPermit.permitId());

        var report =
                queue.report(
                                new NodeReport(
                                        NODE,
                                        1L,
                                        3,
                                        0,
                                        List.of(
                                                new ConnectionRef(
                                                        first.streamName(), first.sessionRid())),
                                        List.of(),
                                        List.of(),
                                        0L),
                                owners -> {
                                    throw new AssertionError("No lease lookup is expected");
                                })
                        .toCompletableFuture()
                        .join();
        assertEquals(List.of(second, third), queue.waitersInOrder());
        assertEquals(Delivery.TICKET, report.deliveries().getFirst().kind());
        assertEquals(second.sessionRid(), report.deliveries().getFirst().sessionRid());
        assertEquals(1, queue.positionOf(third));
        assertFalse(
                queue.admitPermit(
                                permit(
                                        new Connection(
                                                NODE, 2L, first.streamName(), first.sessionRid()),
                                        2))
                        .granted());
        var retry = queue.admitPermit(permit(first, 2));
        assertTrue(retry.granted());
        queue.releasePermit(retry.permitId());
        clock.addAndGet(ZLinkAdmissionQueueState.TICKET_LIFETIME_MS + 1);
        assertFalse(queue.admitPermit(permit(first, 3)).granted());
    }

    private static Connection connection(String session) {
        return new Connection(NODE, 1L, "login", AdmissionQueueFixture.id(session));
    }

    private static AdmitPermit permit(Connection connection, long seq) {
        return new AdmitPermit(
                connection.nodeRid(),
                connection.nodeLifecycleGeneration(),
                "owner",
                1L,
                connection.streamName(),
                connection.sessionRid(),
                EPOCH,
                seq);
    }
}
