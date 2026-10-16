package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import systems.zlink.framework.locationprovider.*;

import java.util.ArrayList;
import java.util.List;

/** Retains authority and capacity records while an owner closes without retaining its lease. */
final class ZLinkEndedOwnerStoreFixture {
    static final String AUTHORITY_PREFIX = "authority\0";
    private static final String CAPACITY_PREFIX = "zlink:v11:capacity:";
    private static final int SCAN_PAGE_SIZE = 100;

    private ZLinkEndedOwnerStoreFixture() {}

    static List<ZLinkStoreScanItem> snapshot(ZLinkLocationStore store) throws Exception {
        var items = new ArrayList<ZLinkStoreScanItem>();
        ZLinkStoreScanCursor cursor = null;
        do {
            var page =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "", cursor, SCAN_PAGE_SIZE),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            for (var item : page.items()) {
                // Preserve shutdown-deleted authority and its capacity accounting, but no
                // lease.
                if (item.key().value().startsWith(AUTHORITY_PREFIX)
                        || item.key().value().startsWith(CAPACITY_PREFIX)) items.add(item);
            }
            cursor = page.nextCursor();
        } while (cursor != null);
        assertFalse(items.isEmpty());
        return items;
    }

    static void restore(ZLinkLocationStore store, List<ZLinkStoreScanItem> items) throws Exception {
        for (var item : items) {
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            item.key(),
                                                            item.value().bytes(),
                                                            null))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            var restored =
                    assertInstanceOf(
                            ZLinkStoreReadFound.class,
                            store.read(item.key(), () -> false).toCompletableFuture().get());
            assertArrayEquals(item.value().bytes(), restored.value().bytes());
        }
    }
}
