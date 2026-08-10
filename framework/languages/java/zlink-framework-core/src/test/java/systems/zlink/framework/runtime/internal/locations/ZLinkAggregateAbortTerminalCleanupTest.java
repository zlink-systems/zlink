package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;

final class ZLinkAggregateAbortTerminalCleanupTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void startupKeepsTombstoneWhenStrictRootDeleteFails() {
        var authority = new CleanupAuthority(false);
        var relocation = new CleanupRelocationStore(true);

        assertThrows(
            CompletionException.class,
            () -> new ZLinkRelocationStartupScanner(
                    authority.repository(), relocation)
                .scanRetainedSessionAborts(OPEN)
                .toCompletableFuture().join());

        assertNotNull(authority.cleanup);
        assertEquals(
            List.of("list-terminal", "inventory", "root-delete"),
            events(authority, relocation));

        assertEquals(
            List.of(),
            new ZLinkRelocationStartupScanner(
                    authority.repository(), relocation)
                .scanRetainedSessionAborts(OPEN)
                .toCompletableFuture().join());
        assertNull(authority.cleanup);
        assertEquals(
            List.of(
                "list-terminal",
                "inventory",
                "root-delete",
                "list-terminal",
                "inventory",
                "root-delete",
                "tombstone",
                "list-retained"),
            events(authority, relocation));
    }

    @Test
    void startupRemovesTombstoneLastAfterRootWasAlreadyDeleted() {
        var authority = new CleanupAuthority(true);
        var relocation = new CleanupRelocationStore(false);

        assertThrows(
            CompletionException.class,
            () -> new ZLinkRelocationStartupScanner(
                    authority.repository(), relocation)
                .scanRetainedSessionAborts(OPEN)
                .toCompletableFuture().join());

        assertNotNull(authority.cleanup);
        assertEquals(false, relocation.rootPresent);

        assertEquals(
            List.of(),
            new ZLinkRelocationStartupScanner(
                    authority.repository(), relocation)
                .scanRetainedSessionAborts(OPEN)
                .toCompletableFuture().join());
        assertNull(authority.cleanup);
        assertEquals(
            List.of(
                "list-terminal",
                "inventory",
                "root-delete",
                "tombstone",
                "list-terminal",
                "inventory",
                "root-delete",
                "tombstone",
                "list-retained"),
            events(authority, relocation));
    }

    private static List<String> events(
        CleanupAuthority authority,
        CleanupRelocationStore relocation) {
        List<Event> events = new ArrayList<>(authority.events);
        events.addAll(relocation.events);
        return events.stream()
            .sorted((left, right) -> Long.compare(left.order(), right.order()))
            .map(Event::name)
            .toList();
    }

    private record Event(long order, String name) {
    }

    private static final class EventOrder {
        private static long next;

        private EventOrder() {
        }

        private static synchronized Event event(String name) {
            return new Event(++next, name);
        }
    }

    private static final class CleanupAuthority implements InvocationHandler {
        private final List<Event> events = new ArrayList<>();
        private boolean failRemoveOnce;
        private ZLinkAggregateAbortCleanupSnapshot cleanup =
            new ZLinkAggregateAbortCleanupSnapshot(
                new ZLinkAggregateFence(UUID.randomUUID(), 1),
                "terminal-version",
                "root-reference",
                17);

        private CleanupAuthority(boolean failRemoveOnce) {
            this.failRemoveOnce = failRemoveOnce;
        }

        private ZLinkLocationRepository repository() {
            return (ZLinkLocationRepository) Proxy.newProxyInstance(
                ZLinkLocationRepository.class.getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                this);
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] arguments) {
            return switch (method.getName()) {
                case "listTerminalAggregateAborts" -> {
                    events.add(EventOrder.event("list-terminal"));
                    yield completed(cleanup == null
                        ? List.of()
                        : List.of(cleanup));
                }
                case "cleanupTerminalAggregateAbortInventory" -> {
                    assertEquals(cleanup, arguments[0]);
                    events.add(EventOrder.event("inventory"));
                    yield completed(true);
                }
                case "removeTerminalAggregateAbort" -> {
                    assertEquals(cleanup, arguments[0]);
                    events.add(EventOrder.event("tombstone"));
                    if (failRemoveOnce) {
                        failRemoveOnce = false;
                        yield CompletableFuture.failedFuture(
                            new IllegalStateException("simulated process stop"));
                    }
                    cleanup = null;
                    yield completed(true);
                }
                case "listRetainedAggregateAborts" -> {
                    events.add(EventOrder.event("list-retained"));
                    yield completed(List.of());
                }
                case "toString" -> "cleanup-authority";
                case "hashCode" -> System.identityHashCode(proxy);
                case "equals" -> proxy == arguments[0];
                default -> throw new UnsupportedOperationException(
                    method.getName());
            };
        }
    }

    private static final class CleanupRelocationStore
        implements ZLinkRelocationStore {
        private final List<Event> events = new ArrayList<>();
        private boolean failDeleteOnce;
        private boolean rootPresent = true;

        private CleanupRelocationStore(boolean failDeleteOnce) {
            this.failDeleteOnce = failDeleteOnce;
        }

        @Override
        public CompletionStage<ZLinkRelocationStored> put(
            byte[] payload,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<ZLinkRelocationReadResult> get(
            String reference,
            ZLinkStoreCancellation cancellation) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<ZLinkRelocationRenewResult> renew(
            String reference,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<ZLinkRelocationDeleteResult> delete(
            String reference,
            ZLinkStoreCancellation cancellation) {
            assertEquals("root-reference", reference);
            events.add(EventOrder.event("root-delete"));
            if (failDeleteOnce) {
                failDeleteOnce = false;
                return CompletableFuture.failedFuture(
                    new IllegalStateException("simulated provider failure"));
            }
            if (!rootPresent) {
                return completed(ZLinkRelocationDeleteResult.MISSING);
            }
            rootPresent = false;
            return completed(ZLinkRelocationDeleteResult.DELETED);
        }
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }
}
