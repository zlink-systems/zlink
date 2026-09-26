/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.eventing.*;
import systems.zlink.contracts.messaging.*;
import systems.zlink.contracts.sockets.*;

/**
 * Projection of the Core ROUTER selected-route observation (Core ROUTER
 * §10.1): routesSnapshot(), POLLROUTE and the route generation carried on a
 * received ROUTER record. Mirrors
 * core/tests/integration/test_router_selected_route_contract.cpp.
 */
class RouterSelectedRouteContractTest {
    private static final Duration WAIT_LIMIT =
        Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS);

    @Test
    void snapshotPollRouteAndRecordGeneration() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = router(context, "route-server");
             RouterSocket client = router(context, "route-client");
             Poller poller = Zlink.createPoller()) {
            String endpoint = bindLoopback(server);
            assertTrue(client.routesSnapshot().isEmpty());

            poller.add(client, 7, PollEventFlags.POLLROUTE);
            connectAs(client, "route-server", endpoint);
            PollEvents events = new PollEvents(1);
            assertEquals(1, poller.wait(events, WAIT_LIMIT));
            assertEquals(7, events.slot(0));
            assertNotEquals(0, events.revents(0) & PollEventFlags.POLLROUTE.mask());

            List<RouterRoute> routes = client.routesSnapshot();
            assertEquals(1, routes.size());
            long selected = findGeneration(routes, "route-server");
            assertNotEquals(0L, selected);
            // A successful snapshot that saw every change clears the level readiness.
            assertFalse(routeReadyNow(poller));

            waitRoute(server, null, "route-client", 0L);
            sendText(server, "route-client", "selected");
            try (Received received = recvText(client, "selected")) {
                assertEquals(selected, received.routeGeneration());
            }

            // A DEALER record carries no ROUTER route generation.
            try (DealerSocket dealer = context.createDealerSocket()) {
                dealer.setRoutingId(RoutingId.from("route-dealer"));
                dealer.options().linger(Duration.ZERO);
                dealer.connect(endpoint);
                waitRoute(server, null, "route-dealer", 0L);
                sendText(server, "route-dealer", "dealer");
                dealer.options().recvTimeout(WAIT_LIMIT);
                try (Received received = new Received()) {
                    assertTrue(dealer.recv(received, RecvFlags.NONE));
                    assertEquals(0L, received.routeGeneration());
                }
            }
        }
    }

    @Test
    void replacedRouteChangesGeneration() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket oldServer = router(context, "route-server");
             RouterSocket client = router(context, "route-client");
             Poller poller = Zlink.createPoller()) {
            RouterSocket newServer = router(context, "route-server");
            try {
                String oldEndpoint = bindLoopback(oldServer);
                String newEndpoint = bindLoopback(newServer);
                poller.add(client, 9, PollEventFlags.POLLROUTE);
                connectAs(client, "route-server", oldEndpoint);
                long first = waitRoute(client, poller, "route-server", 0L);

                // Handover: the new pipe for the same RID takes over the selected route.
                connectAs(client, "route-server", newEndpoint);
                long second = waitRoute(client, poller, "route-server", first);
                assertNotEquals(first, second);

                waitRoute(newServer, null, "route-client", 0L);
                sendText(newServer, "route-client", "new");
                try (Received received = recvText(client, "new")) {
                    assertEquals(second, received.routeGeneration());
                }

                // The selected pipe ends: the retained standby is promoted with a new generation.
                newServer.close();
                newServer = null;
                long promoted = waitRoute(client, poller, "route-server", second);
                assertNotEquals(second, promoted);
            } finally {
                if (newServer != null) {
                    newServer.close();
                }
            }
        }
    }

    @Test
    void snapshotGrowsPastInitialCapacity() {
        TestSupport.assumeNative();
        List<RouterSocket> servers = new ArrayList<>();
        try (Context context = Zlink.createContext();
             RouterSocket client = router(context, "route-client")) {
            try {
                for (int index = 0; index < 20; index++) {
                    String name = "route-server-" + index;
                    RouterSocket server = router(context, name);
                    servers.add(server);
                    connectAs(client, name, bindLoopback(server));
                }
                for (int index = 0; index < 20; index++) {
                    waitRoute(client, null, "route-server-" + index, 0L);
                }
                assertEquals(20, client.routesSnapshot().size());
            } finally {
                for (RouterSocket server : servers) {
                    server.close();
                }
            }
        }
    }

    private static RouterSocket router(Context context, String name) {
        RouterSocket socket = context.createRouterSocket();
        socket.setRoutingId(RoutingId.from(name));
        socket.options().linger(Duration.ZERO);
        socket.options().reconnectInterval(Duration.ZERO);
        socket.options().handover(true);
        return socket;
    }

    private static String bindLoopback(RouterSocket socket) {
        socket.bind("tcp://127.0.0.1:*");
        return socket.options().lastEndpoint();
    }

    private static void connectAs(RouterSocket source, String peer,
                                  String endpoint) {
        source.options().setConnectRoutingId(RoutingId.from(peer));
        source.connect(endpoint);
    }

    private static long findGeneration(List<RouterRoute> routes, String peer) {
        RoutingId expected = RoutingId.from(peer);
        long found = 0L;
        int matches = 0;
        for (RouterRoute route : routes) {
            assertNotEquals(0L, route.routeGeneration());
            if (route.routingId().equals(expected)) {
                matches++;
                found = route.routeGeneration();
            }
        }
        assertTrue(matches <= 1);
        return found;
    }

    // Waits on POLLROUTE until the peer's selected route has a generation
    // other than old; returns it. Core applies route changes while the socket
    // is polled, so a socket without its own poller gets one here.
    private static long waitRoute(RouterSocket socket, Poller poller,
                                  String peer, long old) {
        Poller owned = null;
        if (poller == null) {
            owned = Zlink.createPoller();
            owned.add(socket, 1, PollEventFlags.POLLROUTE);
            poller = owned;
        }
        try {
            long deadline = System.nanoTime() + WAIT_LIMIT.toNanos();
            PollEvents events = new PollEvents(1);
            for (;;) {
                long generation = findGeneration(socket.routesSnapshot(), peer);
                if (generation != 0L && generation != old) {
                    return generation;
                }
                assertTrue(System.nanoTime() < deadline,
                    "route to " + peer + " did not change");
                poller.wait(events, Duration.ofMillis(10));
            }
        } finally {
            if (owned != null) {
                owned.close();
            }
        }
    }

    private static boolean routeReadyNow(Poller poller) {
        PollEvents events = new PollEvents(1);
        int count = poller.wait(events, Duration.ZERO);
        return count == 1
            && (events.revents(0) & PollEventFlags.POLLROUTE.mask()) != 0;
    }

    private static void sendText(RouterSocket socket, String peer, String text) {
        socket.send(RoutingId.from(peer)).message(Message.from(text)).submit_sync();
    }

    private static Received recvText(RouterSocket socket, String expected) {
        socket.options().recvTimeout(WAIT_LIMIT);
        Received received = new Received();
        assertTrue(socket.recv(received, RecvFlags.NONE));
        assertEquals(1, received.parts().size());
        assertEquals(expected, received.parts().get(0).toUtf8String());
        return received;
    }
}
