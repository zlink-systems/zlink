package systems.zlink.e2e.observabilityops.client;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeoutException;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class ObservabilityScenarioSupport {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(30);
    private static final long ISOLATION_DELAY_MILLIS = 350;
    private static final long ACTOR_TIMER_ISOLATION_DELAY_MILLIS = 5000;
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ClientOptions options;

    public ObservabilityScenarioSupport(ClientOptions options) {
        this.options = options;
    }

    public void runBasicTerminator(ZLinkStreamConnector connector) throws Exception {
        runScenario(connector, "ATD-A1", List.of(
            "hold-started",
            "probe-started",
            "probe-completed",
            "hold-resumed",
            "hold-completed"));
    }

    public void runTimerIsolation(ZLinkStreamConnector connector) throws Exception {
        String requestId = "obs-timer-" + System.nanoTime();
        String timerSpot = requestId + "-spot";
        Contracts.EnsureSpotRes ensured = connector
            .request(new Contracts.EnsureSpotReq(timerSpot))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
        ensure(Contracts.PLAY_NODE_A.equals(ensured.nodeRid()),
            "observability timer spot was not placed on Play-A");
        Map<String, String> metadata = Map.of(
            Contracts.SPOT_RID_METADATA, timerSpot);
        String playEvidence = options.playHttpEndpoint() + "/evidence";
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                requestId + "-await",
                "observability-fanout",
                50,
                ISOLATION_DELAY_MILLIS))
            .metadata(metadata)
            .submit();
        connector
            .send(new Contracts.TimerStartMsg(
                requestId,
                requestId + "-fast",
                "fast",
                100,
                0))
            .metadata(metadata)
            .submit();
        assertOrder(playEvidence, requestId,
            List.of("obs-fanout-published", "timer-fast-completed"));
        connector
            .send(new Contracts.TimerStopMsg(requestId))
            .metadata(metadata)
            .submit();
    }

    public void runSessionRelayActorAwait(ZLinkStreamConnector connector) throws Exception {
        String requestId = "obs-a1-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes bind = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bind.actorA()), "OBS-A1 actor A bind mismatch");

        ZLinkStreamConnector unbound = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(options.streamEndpoint())));
        try {
            unbound.connect().submit().toCompletableFuture().join();
            CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> push = connector
                .waitFor(Contracts.ActorPushNotify.class)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorPushAwaitRes reply = connector
                .request(new Contracts.ActorPushAwaitReq(requestId, 350, "bound-session-push"))
                .metadata(Contracts.ACTOR_ID_METADATA, actorA)
                .timeout(REQUEST_TIMEOUT)
                .submit(Contracts.ActorPushAwaitRes.class).toCompletableFuture().join();
            connector.dispatch().submit().toCompletableFuture().join();
            Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();
            ensure(actorA.equals(reply.actorId()), "OBS-A1 reply actor mismatch");
            ensure("actor-push-await-completed".equals(reply.marker()), "OBS-A1 reply marker mismatch");
            ensure(actorA.equals(notify.actorId()), "OBS-A1 push actor mismatch");
            ensure(requestId.equals(notify.requestId()), "OBS-A1 push request mismatch");
            ensure("bound-session-push".equals(notify.value()), "OBS-A1 push value mismatch");
            ensure(!notify.nodeRid().isBlank(), "OBS-A1 push node missing");
            CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> unboundPush = unbound
                .waitFor(Contracts.ActorPushNotify.class)
                .timeout(Duration.ofMillis(400))
                .submit(Contracts.ActorPushNotify.class);
            unbound.dispatch().submit().toCompletableFuture().join();
            expectFailure(() -> unboundPush.toCompletableFuture().join(), "OBS-A1 unbound session received actor push");
        } finally {
            unbound.close().submit().toCompletableFuture().join();
        }
    }

    public void runObservabilityTransfer(ZLinkStreamConnector connector) throws Exception {
        String requestId = "obsb2-" + System.nanoTime();
        String remoteSpot = requestId + "-remote-spot";
        Contracts.EnsureSpotRes ensured = connector
            .request(new Contracts.EnsureSpotReq(remoteSpot))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class).toCompletableFuture().join();
        ensure(Contracts.PLAY_NODE_B.equals(ensured.nodeRid()), "OBS-B2 remote spot node mismatch");

        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes bound = connector
            .request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        ensure(actorA.equals(bound.actorA()), "OBS-B2 actor bind mismatch");

        Contracts.ActorJoinRes joined = connector
            .request(new Contracts.ActorJoinReq(requestId, remoteSpot))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
        ensure(actorA.equals(joined.actorId()), "OBS-B2 transferred actor mismatch");
        System.out.println("scenario OBS-B2 passed actor=" + actorA + " spot=" + remoteSpot);
    }

    public void runObservabilityQueue(ZLinkStreamConnector connector) throws Exception {
        String requestId = "obsb2q-" + System.nanoTime();
        ZLinkStreamConnector peer = ZLinkStreamConnectorFactory.create(connector.options());
        try {
            peer.connect().submit().toCompletableFuture().join();
            CompletionStage<Contracts.ScenarioRes> first = connector
                .request(new Contracts.ScenarioReq("OBS-B2-QUEUE", requestId + "-1"))
                .timeout(REQUEST_TIMEOUT).submit(Contracts.ScenarioRes.class);
            Thread.sleep(50);
            CompletionStage<Contracts.ScenarioRes> second = peer
                .request(new Contracts.ScenarioReq("OBS-B2-QUEUE", requestId + "-2"))
                .timeout(REQUEST_TIMEOUT).submit(Contracts.ScenarioRes.class);
            first.toCompletableFuture().join();
            second.toCompletableFuture().join();
        } finally {
            peer.close().submit().toCompletableFuture().join();
        }
        System.out.println("scenario OBS-B2-QUEUE passed");
    }

    public void runObservabilityDrainHandoff(ZLinkStreamConnector connector) throws Exception {
        String requestId = "obsc2-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        connector.request(new Contracts.BindActorsReq(Contracts.TARGET_SPOT, actorA, actorB))
            .timeout(REQUEST_TIMEOUT).submit(Contracts.BindActorsRes.class).toCompletableFuture().join();
        CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> push = connector
            .waitFor(Contracts.ActorPushNotify.class).timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorPushNotify.class);
        CompletionStage<Contracts.ActorPushAwaitRes> pending = connector
            .request(new Contracts.ActorPushAwaitReq(requestId, 3000, "drain-bound-push"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(Duration.ofSeconds(8)).submit(Contracts.ActorPushAwaitRes.class);
        System.out.println("OBS-C2 pending-started actor=" + actorA);
        System.out.flush();
        boolean pendingCompleted;
        try {
            pending.toCompletableFuture().join();
            pendingCompleted = true;
        } catch (CompletionException timeout) {
            pendingCompleted = timeout.getCause() instanceof TimeoutException;
        }
        waitForMetricValue(
            options.playHttpEndpoint() + "/metrics",
            "zlink.drain.actors.handed_off",
            2);
        Contracts.ActorPushAwaitRes reply = connector
            .request(new Contracts.ActorPushAwaitReq(requestId + "-after", 100, "drain-bound-push"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorA)
            .timeout(REQUEST_TIMEOUT).submit(Contracts.ActorPushAwaitRes.class)
            .toCompletableFuture().join();
        connector.dispatch().submit().toCompletableFuture().join();
        Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();
        ensure(actorA.equals(reply.actorId()), "OBS-C2 pending reply actor mismatch");
        ensure(actorA.equals(notify.actorId()), "OBS-C2 bound push actor mismatch");
        System.out.println("OBS-C2 pending-completed=" + pendingCompleted + " bound-push=true");
    }

    public void runPersistentRoomWrite(ZLinkStreamConnector connector) {
        connector.request(new Contracts.EnsureSpotReq("obs-c3-persistent-room"))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
            .timeout(REQUEST_TIMEOUT).submit(Contracts.EnsureSpotRes.class)
            .toCompletableFuture().join();
        Contracts.PersistentRoomStateRes state = persistentRoom(
            connector, Contracts.PLAY_NODE_A,
            new Contracts.PersistentRoomStateReq("state-v1", true));
        ensure("state-v1".equals(state.value()), "OBS-C3 write state mismatch");
        System.out.println("OBS-C3 write node=" + state.nodeRid()
            + " events=" + state.eventCount() + " value=" + state.value());
    }

    public void runPersistentRoomRead(ZLinkStreamConnector connector) {
        String room = "obs-c3-persistent-room";
        Contracts.EnsureSpotRes created = connector
            .request(new Contracts.EnsureSpotReq(room))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B)
            .timeout(REQUEST_TIMEOUT).submit(Contracts.EnsureSpotRes.class)
            .toCompletableFuture().join();
        Contracts.PersistentRoomStateRes state = persistentRoom(
            connector, Contracts.PLAY_NODE_B,
            new Contracts.PersistentRoomStateReq("", false));
        ensure(Contracts.PLAY_NODE_B.equals(created.nodeRid()), "OBS-C3 recreate node mismatch");
        ensure("state-v1".equals(state.value()), "OBS-C3 replay state mismatch");
        ensure(state.replayed(), "OBS-C3 state was not replayed");
        System.out.println("OBS-C3 read node=" + state.nodeRid()
            + " events=" + state.eventCount() + " value=" + state.value()
            + " replayed=" + state.replayed());
    }

    private Contracts.PersistentRoomStateRes persistentRoom(
        ZLinkStreamConnector connector,
        String nodeRid,
        Contracts.PersistentRoomStateReq request) {
        String room = "obs-c3-persistent-room";
        return connector.request(request)
            .metadata(Contracts.SPOT_RID_METADATA, room)
            .metadata(Contracts.TARGET_NODE_RID_METADATA, nodeRid)
            .timeout(REQUEST_TIMEOUT).submit(Contracts.PersistentRoomStateRes.class)
            .toCompletableFuture().join();
    }

    public void runDrainRolloutBind(ZLinkStreamConnector connector) {
        Contracts.BindActorsRes bound = connector
            .request(new Contracts.BindActorsReq(
                Contracts.TARGET_SPOT, "obs-c5-actor-a", "obs-c5-actor-b"))
            .timeout(REQUEST_TIMEOUT).submit(Contracts.BindActorsRes.class)
            .toCompletableFuture().join();
        System.out.println("OBS-C5 bound=" + bound.actors().size()
            + " source=" + bound.actors().get(0).nodeRid());
    }

    public void runDrainTargetProbe(ZLinkStreamConnector connector) {
        Contracts.EnsureSpotRes ready = connector
            .request(new Contracts.EnsureSpotReq("obs-c5-target-ready"))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_B)
            .timeout(REQUEST_TIMEOUT).submit(Contracts.EnsureSpotRes.class)
            .toCompletableFuture().join();
        ensure(Contracts.PLAY_NODE_B.equals(ready.nodeRid()), "OBS-C5 target route mismatch");
        System.out.println("OBS-C5 target-ready=" + ready.nodeRid());
    }

    public void runRelocationWorkloadPrepare(
        ZLinkStreamConnector connector,
        String scenarioId) throws Exception {
        String normalized = scenarioId.toLowerCase().replaceAll("[^a-z0-9]+", "-");
        String spotRid = "obs-" + normalized + "-room";
        String actorId = "obs-" + normalized + "-actor";
        Contracts.EnsureSpotRes ensured = connector
            .request(new Contracts.EnsureSpotReq(spotRid))
            .metadata(Contracts.TARGET_NODE_RID_METADATA, Contracts.PLAY_NODE_A)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class)
            .toCompletableFuture().join();
        ensure(Contracts.PLAY_NODE_A.equals(ensured.nodeRid()),
            scenarioId + " source spot placement mismatch: " + ensured.nodeRid());
        Contracts.BindActorsRes bound = connector
            .request(new Contracts.BindActorsReq(spotRid, actorId, actorId))
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.BindActorsRes.class)
            .toCompletableFuture().join();
        ensure(actorId.equals(bound.actorA()), scenarioId + " actor bind mismatch");
        Contracts.ActorJoinRes joined = connector
            .request(new Contracts.ActorJoinReq(scenarioId + "-join", spotRid))
            .metadata(Contracts.ACTOR_ID_METADATA, actorId)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorJoinRes.class)
            .toCompletableFuture().join();
        ensure(actorId.equals(joined.actorId()), scenarioId + " actor join mismatch");
        // ActorJoin is accepted before the asynchronous target route update
        // completes. Give that public route transition time to settle before
        // probing the target handler.
        Thread.sleep(2_000L);
        System.out.println(scenarioId + " prepared spot=" + spotRid
            + " actor=" + actorId + " generation=" + bound.actors().getFirst().generation());
        System.out.flush();
        CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> push = connector
            .waitFor(Contracts.ActorPushNotify.class)
            .timeout(Duration.ofSeconds(45))
            .submit(Contracts.ActorPushNotify.class);
        try {
            connector.request(new Contracts.ActorPushAwaitReq(
                    scenarioId, 3_000, scenarioId + "-pending"))
                .metadata(Contracts.ACTOR_ID_METADATA, actorId)
                .timeout(Duration.ofSeconds(45))
                .submit(Contracts.ActorPushAwaitRes.class)
                .toCompletableFuture().join();
            connector.dispatch().submit().toCompletableFuture().join();
            Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();
            System.out.println(scenarioId + " pending-completed node=" + notify.nodeRid());
        } catch (RuntimeException error) {
            System.out.println(scenarioId + " pending-terminal=" + error.getClass().getSimpleName());
        }
        if (!options.relocationReleaseFile().isBlank()) {
            Path release = Path.of(options.relocationReleaseFile());
            long deadline = System.nanoTime() + Duration.ofSeconds(90).toNanos();
            while (!Files.exists(release) && System.nanoTime() < deadline) {
                Thread.sleep(50L);
            }
            if (!Files.exists(release)) {
                throw new IllegalStateException(
                    "relocation release file was not created: " + release);
            }
            runRelocationWorkloadAfter(connector, scenarioId);
        }
    }

    public void runRelocationWorkloadAfter(
        ZLinkStreamConnector connector,
        String scenarioId) {
        String normalized = scenarioId.toLowerCase().replaceAll("[^a-z0-9]+", "-");
        String actorId = "obs-" + normalized + "-actor";
        CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> push = connector
            .waitFor(Contracts.ActorPushNotify.class)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorPushNotify.class);
        connector.request(new Contracts.ActorPushAwaitReq(
                scenarioId + "-after", 50, scenarioId + "-after"))
            .metadata(Contracts.ACTOR_ID_METADATA, actorId)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ActorPushAwaitRes.class)
            .toCompletableFuture().join();
        connector.dispatch().submit().toCompletableFuture().join();
        Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();
        ensure(actorId.equals(notify.actorId()), scenarioId + " after actor mismatch");
        System.out.println(scenarioId + " after node=" + notify.nodeRid()
            + " actor=" + notify.actorId());
    }

    private void runScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        List<String> expectedOrder) throws Exception {
        runScenario(connector, scenarioId, expectedOrder, Map.of(), List.of());
    }

    private void runScenario(
        ZLinkStreamConnector connector,
        String scenarioId,
        List<String> expectedOrder,
        Map<String, String> metadata,
        List<String> expectedValueFragments) throws Exception {
        String requestId = scenarioId.toLowerCase().replace("-", "") + "-" + System.nanoTime();
        Contracts.ScenarioRes reply = connector
            .request(new Contracts.ScenarioReq(scenarioId, requestId))
            .metadata(metadata)
            .timeout(REQUEST_TIMEOUT)
            .submit(Contracts.ScenarioRes.class).toCompletableFuture().join();
        ensure(scenarioId.equals(reply.scenarioId()), scenarioId + " reply scenario mismatch");
        ensure(requestId.equals(reply.requestId()), scenarioId + " reply request id mismatch");
        assertOrder(requestId, expectedOrder);
        for (String valueFragment : expectedValueFragments) {
            assertAllValuesContain(requestId, expectedOrder, valueFragment);
        }
    }

    private void assertOrder(String requestId, List<String> expectedOrder) throws Exception {
        assertOrder(options.playHttpEndpoint() + "/evidence", requestId, expectedOrder);
    }

    private void assertOrder(String evidenceUrl, String requestId, List<String> expectedOrder)
        throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            List<String> observed = observedMarkers(evidenceUrl, requestId);
            if (containsInOrder(observed, expectedOrder)) {
                return;
            }
            Thread.sleep(100);
        }
        throw new IllegalStateException(
            "expected marker order " + expectedOrder + " for " + requestId
                + ", observed=" + observedMarkers(evidenceUrl, requestId));
    }

    private void assertOrder(String[] evidenceUrls, String requestId, List<String> expectedOrder)
        throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        List<String> observed = List.of();
        while (System.nanoTime() < deadline) {
            for (String evidenceUrl : evidenceUrls) {
                observed = observedMarkers(evidenceUrl, requestId);
                if (containsInOrder(observed, expectedOrder)) {
                    return;
                }
            }
            Thread.sleep(100);
        }
        throw new IllegalStateException(
            "expected marker order " + expectedOrder + " for " + requestId
                + ", observed=" + observed);
    }

    private List<String> observedMarkers(String requestId) throws Exception {
        return observedMarkers(options.playHttpEndpoint() + "/evidence", requestId);
    }

    private List<String> observedMarkers(String evidenceUrl, String requestId) throws Exception {
        JsonNode root = JSON.readTree(get(evidenceUrl));
        List<String> markers = new ArrayList<>();
        for (JsonNode entry : root.path("entries")) {
            if (requestId.equals(entry.path("subject").asText())) {
                markers.add(entry.path("marker").asText());
            }
        }
        return markers;
    }

    private void assertAllValuesContain(
        String requestId,
        List<String> expectedMarkers,
        String valueFragment) throws Exception {
        assertAllValuesContain(
            options.playHttpEndpoint() + "/evidence",
            requestId,
            expectedMarkers,
            valueFragment);
    }

    private void assertAllValuesContain(
        String evidenceUrl,
        String requestId,
        List<String> expectedMarkers,
        String valueFragment) throws Exception {
        JsonNode root = JSON.readTree(get(evidenceUrl));
        for (JsonNode entry : root.path("entries")) {
            String marker = entry.path("marker").asText();
            if (requestId.equals(entry.path("subject").asText()) && expectedMarkers.contains(marker)) {
                String value = entry.path("value").asText();
                ensure(value.contains(valueFragment),
                    "expected " + marker + " value to contain " + valueFragment + ", value=" + value);
            }
        }
    }

    private void assertAllValuesContain(
        String[] evidenceUrls,
        String requestId,
        List<String> expectedMarkers,
        String valueFragment) throws Exception {
        for (String evidenceUrl : evidenceUrls) {
            assertAllValuesContain(evidenceUrl, requestId, expectedMarkers, valueFragment);
        }
    }

    private void assertNoMarker(String evidenceUrl, String requestId, String marker) throws Exception {
        List<String> observed = observedMarkers(evidenceUrl, requestId);
        ensure(!observed.contains(marker), "unexpected marker " + marker + " for " + requestId);
    }

    private void waitForMetricValue(String metricsUrl, String metricName, double minimum)
        throws Exception {
        long deadline = System.nanoTime() + REQUEST_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            JsonNode rows = JSON.readTree(get(metricsUrl));
            for (JsonNode row : rows) {
                if (metricName.equals(row.path("name").asText())
                    && row.path("value").asDouble() >= minimum) {
                    return;
                }
            }
            Thread.sleep(100);
        }
        throw new IllegalStateException(
            "timed out waiting for " + metricName + " >= " + minimum);
    }

    private interface ThrowingRunnable {
        void run() throws Exception;
    }

    private void expectFailure(ThrowingRunnable action, String message) throws Exception {
        try {
            action.run();
        } catch (Exception error) {
            if (error instanceof ExecutionException && error.getCause() != null) {
                return;
            }
            return;
        }
        throw new IllegalStateException(message);
    }

    private static boolean containsInOrder(List<String> observed, List<String> expected) {
        int index = 0;
        for (String marker : observed) {
            if (index < expected.size() && expected.get(index).equals(marker)) {
                index++;
            }
        }
        return index == expected.size();
    }

    private String get(String url) throws Exception {
        URI target = URI.create(url);
        String baseUrl = target.getScheme() + "://" + target.getRawAuthority();
        String path = target.getRawPath();
        if (target.getRawQuery() != null) {
            path += "?" + target.getRawQuery();
        }
        RawHttpResponse response = ZLinkHttpClient.create(baseUrl)
            .timeout(Duration.ofSeconds(3))
            .get(path)
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() >= 200 && response.status() < 300,
            "GET " + url + " returned " + response.status());
        return response.body();
    }

    private void ensure(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
