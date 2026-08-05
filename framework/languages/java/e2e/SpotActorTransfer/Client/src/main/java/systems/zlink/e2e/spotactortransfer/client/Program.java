package systems.zlink.e2e.spotactortransfer.client;

import com.fasterxml.jackson.databind.JsonNode;
import java.net.URI;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.nio.file.Files;
import java.nio.file.Path;
import systems.zlink.e2e.spotactortransfer.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.stream.connector.ZLinkStreamCompression;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamPacketNameResolver;

public final class Program implements AutoCloseable {
    private final ClientOptions options;
    private final String nodeA;
    private final String nodeB;
    private final String nodeC;
    private final ZLinkHttpClient nodeAHttp;
    private final ZLinkHttpClient nodeBHttp;
    private final ZLinkHttpClient nodeCHttp;
    private final Path signals;

    private Program(ClientOptions options) {
        this.options = options;
        nodeA = options.nodeAHttpEndpoint(); nodeB = options.nodeBHttpEndpoint(); nodeC = options.nodeCHttpEndpoint();
        nodeAHttp = createHttpClient(nodeA); nodeBHttp = createHttpClient(nodeB); nodeCHttp = createHttpClient(nodeC);
        signals = Path.of(options.logDirectory());
    }

    public static void main(String... args) throws Exception {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: spot-actor-transfer-client --config <path> --scenario <selector>");
        }
        systems.zlink.e2e.spotactortransfer.shared.Env.configure(args);
        try (Program program = new Program(ClientOptions.load(args[1]))) {
            String selected = args[3];
            for (String scenario : scenarios(selected)) {
                program.run(scenario);
                System.out.println("scenario " + scenario + " passed");
            }
            System.out.println("spot-actor-transfer e2e result=passed");
        }
    }

    private static List<String> scenarios(String selected) {
        List<String> implemented = List.of(
            "ST-A1", "ST-A2", "ST-A3", "ST-B1", "ST-B2", "ST-B3", "ST-B4",
            "ST-C1", "ST-C2", "ST-C3", "ST-D1", "ST-D2", "ST-E1", "ST-E2",
            "ST-F1", "ST-F2", "ST-F3", "ST-F4", "ST-F5", "ST-F6", "ST-R1");
        if ("all".equalsIgnoreCase(selected)) {
            return implemented;
        }
        return java.util.Arrays.stream(selected.split(","))
            .map(String::trim)
            .filter(value -> !value.isEmpty())
            .map(value -> value.toUpperCase(java.util.Locale.ROOT))
            .toList();
    }

    private void run(String scenario) throws Exception {
        switch (scenario) {
            case "ST-A1" -> localAccept();
            case "ST-A2" -> localReject();
            case "ST-A3" -> movingDispatch();
            case "ST-B1" -> remoteStateful();
            case "ST-B2" -> committedTransferSurvivesSourceLoss("ST-B2");
            case "ST-B3" -> remoteNoAdapter();
            case "ST-B4" -> remoteEmptyState();
            case "ST-C1" -> sourceDownBeforeCommit();
            case "ST-C2" -> committedTransferSurvivesSourceLoss("ST-C2");
            case "ST-C3" -> callbackFailures();
            case "ST-D1" -> locationCommitTiming();
            case "ST-D2" -> committedTransferSurvivesSourceLoss("ST-D2");
            case "ST-E1" -> boundSessionTransfer();
            case "ST-E2" -> failedTransferKeepsBoundSession();
            case "ST-F1" -> inFlightHandoffOrder();
            case "ST-F2" -> directDoesNotOvertake();
            case "ST-F3" -> boundSessionCrossMoveOrder();
            case "ST-F4" -> messageFollowDuration();
            case "ST-F5" -> messageFollowRouteRemoval();
            case "ST-F6" -> inFlightRequestReplyCorrelationAndTimeout();
            case "ST-R1" -> retireAcceptedRequestAndBoundSession();
            default -> throw new IllegalArgumentException("unsupported scenario: " + scenario);
        }
    }

    private void retireAcceptedRequestAndBoundSession() throws Exception {
        Contracts.ActorCreateRes created = null;
        String actorId = null;
        for (int attempt = 0; attempt < 40; attempt++) {
            String candidateId = id("actor-retire-accepted");
            try {
                Contracts.ActorCreateRes candidate =
                    createActor(nodeA, candidateId, Contracts.STATEFUL, 101);
                if ("actor-a".equals(candidate.nodeRid())) {
                    created = candidate;
                    actorId = candidateId;
                    break;
                }
            } catch (Exception notReady) {
                Thread.sleep(200);
            }
        }
        require(created != null,
            "ST-R1 could not allocate an Actor on the retiring node");
        final String selectedActorId = actorId;
        ZLinkStreamConnector connector = connector(options.streamCEndpoint());
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.BindSessionRes bound = bindSessionEventually(
                connector,
                selectedActorId,
                Duration.ofSeconds(15));
            require("actor-a".equals(bound.nodeRid()),
                "ST-R1 Session did not bind the source Actor");

            CompletableFuture<Contracts.RetireRes> retire =
                postAsyncWithTimeout(
                    nodeA + "/runtime/retire",
                    java.util.Map.of(),
                    Contracts.RetireRes.class,
                    Duration.ofSeconds(40));
            waitForRetireCapture(
                selectedActorId,
                retire,
                Duration.ofSeconds(15));

            CompletableFuture<Contracts.ProbeRes> accepted =
                probeAsync(
                    nodeA,
                    selectedActorId,
                    "ST-R1",
                    "accepted-during-retire");
            Thread.sleep(250);
            release(nodeA, selectedActorId);

            Contracts.ProbeRes reply = accepted.get(20, TimeUnit.SECONDS);
            require(!"actor-a".equals(reply.nodeRid()),
                "ST-R1 accepted request executed on the retired source");
            Contracts.RetireRes terminal = retire.get(40, TimeUnit.SECONDS);
            require("RELOCATE_THEN_SHUTDOWN".equals(terminal.intent()),
                "ST-R1 returned a different termination intent");
            require("STOPPED".equals(terminal.outcome())
                    && "NONE".equals(terminal.reason()),
                "ST-R1 retire failed: " + terminal);

            assertBoundPush(
                connector,
                selectedActorId,
                "ST-R1",
                "after-retire-route-ack",
                reply.nodeRid());
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    private void waitForRetireCapture(
        String actorId,
        CompletableFuture<Contracts.RetireRes> retire,
        Duration timeout) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (System.nanoTime() < deadline) {
            if (hasKind(evidence(), actorId, "retire_capture_gate")) {
                return;
            }
            if (retire.isDone()) {
                Contracts.RetireRes terminal = retire.get();
                throw new AssertionError(
                    "ST-R1 retire completed before Actor capture: "
                        + terminal);
            }
            Thread.sleep(50);
        }
        throw new AssertionError(
            "timed out waiting for retire_capture_gate actor=" + actorId);
    }

    private Contracts.BindSessionRes bindSessionEventually(
        ZLinkStreamConnector connector,
        String actorId,
        Duration timeout) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        Throwable lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return connector
                    .request(new Contracts.BindSessionReq("ST-R1", actorId))
                    .submit(Contracts.BindSessionRes.class)
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            } catch (Exception failure) {
                lastFailure = failure;
                Thread.sleep(100);
            }
        }
        throw new AssertionError(
            "ST-R1 Session could not bind after topology convergence",
            lastFailure);
    }

    private void localAccept() throws Exception {
        String spotRid = id("spot-local-ok");
        Contracts.CreateSpotRes createdSpot =
            createSpot(nodeA, spotRid, "accept");
        String actorId = null;
        for (int attempt = 0; attempt < 40; attempt++) {
            String candidateId = id("actor-local-ok");
            Contracts.ActorCreateRes candidate =
                createActor(nodeA, candidateId, Contracts.STATEFUL, 11);
            if (createdSpot.nodeRid().equals(candidate.nodeRid())) {
                actorId = candidateId;
                break;
            }
        }
        require(actorId != null,
            "ST-A1 could not allocate an Actor on the target Spot node");
        Contracts.JoinTargetRes join = join(nodeA, actorId, "ST-A1", spotRid, "accept");
        require(join.accepted(), "ST-A1 join was rejected");
        Contracts.ProbeRes probe = probe(nodeA, actorId, "ST-A1", "after-joined");
        require(createdSpot.nodeRid().equals(probe.nodeRid()),
            "ST-A1 packet did not stay on the target Spot node");
        require(spotRid.equals(probe.spotRid()), "ST-A1 packet did not reach target spot");
        List<Contracts.Evidence> entries = evidence();
        require(hasKind(entries, actorId, "admission"),
            "ST-A1 target admission was not observed");
        require(hasKind(entries, actorId, "joined"),
            "ST-A1 target membership callback was not observed");
        require(hasKind(entries, actorId, "packet_handler"),
            "ST-A1 post-join request was not dispatched");
    }

    private void localReject() throws Exception {
        String actorId = id("actor-local-reject");
        String spotRid = id("spot-local-reject");
        createSpot(nodeA, spotRid, "reject");
        createActor(nodeA, actorId, Contracts.STATEFUL, 12);
        Contracts.JoinTargetRes join = join(nodeA, actorId, "ST-A2", spotRid, "reject");
        require(!join.accepted(), "ST-A2 join should be rejected");
        Contracts.ProbeRes probe = probe(nodeA, actorId, "ST-A2", "after-reject");
        require("actor-a".equals(probe.spotRid()),
            "ST-A2 source membership changed after rejection");
        List<Contracts.Evidence> evidence = evidence();
        require(noKind(evidence, actorId, "leave"), "ST-A2 leave side effect exists");
        require(noKind(evidence, actorId, "joined"), "ST-A2 joined side effect exists");
    }

    private void movingDispatch() throws Exception {
        String actorId = id("actor-local-moving");
        String spotRid = id("spot-local-moving");
        createSpot(nodeA, spotRid, "delay-joined");
        createActor(nodeA, actorId, Contracts.STATEFUL, 13);
        CompletableFuture<Contracts.JoinTargetRes> join = joinAsync(
            nodeA, actorId, "ST-A3", spotRid, "accept");
        waitFor(actorId, "joined_wait", Duration.ofSeconds(8));
        CompletableFuture<Contracts.ProbeRes> blocked = probeAsync(
            nodeA, actorId, "ST-A3", "during-joined-wait");
        Thread.sleep(250);
        require(!blocked.isDone(), "ST-A3 packet completed before joined callback");
        release(nodeA, spotRid);
        require(join.get(8, TimeUnit.SECONDS).accepted(), "ST-A3 join was rejected");
        Contracts.ProbeRes probe = blocked.get(8, TimeUnit.SECONDS);
        require(spotRid.equals(probe.spotRid()), "ST-A3 blocked packet did not resume on target");
    }

    private void remoteStateful() throws Exception {
        RemoteActorFixture fixture = createRemoteActor(
            "remote-ok", Contracts.STATEFUL, 21, "accept", null);
        require(join(fixture.sourceHttp(), fixture.actorId(), "ST-B1", fixture.spotRid(), "accept").accepted(),
            "ST-B1 remote join failed");
        Contracts.ProbeRes probe = probe(
            fixture.targetHttp(), fixture.actorId(), "ST-B1", "after-transfer");
        require(fixture.targetNodeRid().equals(probe.nodeRid()),
            "ST-B1 target owner changed during transfer");
        require(probe.stateVersion() == 21, "ST-B1 state was not restored");
        waitFor(fixture.actorId(), "location_committed", Duration.ofSeconds(3));
        waitFor(fixture.actorId(), "message_follow_registered", Duration.ofSeconds(3));
        assertNodeOrder(fixture.actorId(), fixture.sourceNodeRid(), List.of(
            "transfer_out", "leave", "commit_request", "location_visible", "success_reply"));
        assertNodeOrder(fixture.actorId(), fixture.targetNodeRid(), List.of(
            "admission", "transfer_in", "joined", "commit_ack"));
        assertCorrelatedTransferMarkers(fixture.actorId(), List.of(
            "commit_request", "location_committed", "message_follow_registered", "commit_ack"));
    }

    private void inFlightHandoffOrder() throws Exception {
        HandoffFixture fixture = startHandoff("ST-F1");
        List<CompletableFuture<Contracts.ProbeRes>> backlog = new ArrayList<>();
        for (String marker : List.of("P1", "P2", "P3")) {
            backlog.add(probeAsync(fixture.targetHttp(), fixture.actorId(), "ST-F1", marker));
            Thread.sleep(75);
        }
        Thread.sleep(200);
        require(backlog.stream().noneMatch(CompletableFuture::isDone),
            "ST-F1 packet completed while actor was moving");
        release(fixture.targetHttp(), fixture.spotRid());
        require(fixture.join().get(8, TimeUnit.SECONDS).accepted(), "ST-F1 transfer failed");
        for (CompletableFuture<Contracts.ProbeRes> response : backlog) {
            require(fixture.targetNodeRid().equals(response.get(8, TimeUnit.SECONDS).nodeRid()),
                "ST-F1 backlog did not replay on target");
        }
        assertMarkerOrder(fixture.actorId(), "packet_handler", List.of("P1", "P2", "P3"));
        assertNodeOrder(
            fixture.actorId(), fixture.targetNodeRid(),
            List.of("target_backlog_replayed", "packet_handler"));
    }

    private void directDoesNotOvertake() throws Exception {
        HandoffFixture fixture = startHandoff("ST-F2");
        CompletableFuture<Contracts.ProbeRes> b1 = probeAsync(
            fixture.targetHttp(), fixture.actorId(), "ST-F2", "B1");
        Thread.sleep(75);
        CompletableFuture<Contracts.ProbeRes> b2 = probeAsync(
            fixture.targetHttp(), fixture.actorId(), "ST-F2", "B2");
        Thread.sleep(150);
        release(fixture.targetHttp(), fixture.spotRid());
        require(fixture.join().get(8, TimeUnit.SECONDS).accepted(), "ST-F2 transfer failed");
        CompletableFuture<Contracts.ProbeRes> direct = probeAsync(
            fixture.targetHttp(), fixture.actorId(), "ST-F2", "D1");
        b1.get(8, TimeUnit.SECONDS);
        b2.get(8, TimeUnit.SECONDS);
        direct.get(8, TimeUnit.SECONDS);
        assertMarkerOrder(fixture.actorId(), "packet_handler", List.of("B1", "B2", "D1"));
        assertNodeOrder(
            fixture.actorId(), fixture.targetNodeRid(),
            List.of("target_backlog_replayed", "packet_handler"));
    }

    private void boundSessionCrossMoveOrder() throws Exception {
        String actorId = id("actor-f3");
        String spotRid = id("spot-f3");
        Contracts.ActorCreateRes created =
            createActor(nodeA, actorId, Contracts.STATEFUL, 93);
        Contracts.CreateSpotRes target = createSpot(nodeB, spotRid, "delay-joined");
        String sourceHttp = httpEndpoint(created.nodeRid());
        String targetHttp = httpEndpoint(target.nodeRid());
        ZLinkStreamConnector connector = connector(streamEndpoint(created.nodeRid()));
        try {
            connector.connect().submit().toCompletableFuture().join();
            connector.request(new Contracts.BindSessionReq("ST-F3", actorId))
                .submit(Contracts.BindSessionRes.class).toCompletableFuture().join();
            CompletableFuture<Contracts.JoinTargetRes> join = joinAsync(
                sourceHttp, actorId, "ST-F3", spotRid, "accept");
            waitFor(actorId, "joined_wait", Duration.ofSeconds(8));
            List<CompletableFuture<Contracts.BoundPushRes>> packets = new ArrayList<>();
            for (String marker : List.of("S1", "S2", "S3", "S4")) {
                packets.add(CompletableFuture.supplyAsync(() -> {
                    try {
                        return connector
                            .request(new Contracts.BoundPushReq("ST-F3", marker))
                            .metadata("actor-id", actorId)
                            .submit(Contracts.BoundPushRes.class).toCompletableFuture().join();
                    } catch (Exception error) {
                        throw new java.util.concurrent.CompletionException(error);
                    }
                }));
                Thread.sleep(75);
            }
            Thread.sleep(200);
            release(targetHttp, spotRid);
            require(join.get(8, TimeUnit.SECONDS).accepted(), "ST-F3 transfer failed");
            for (CompletableFuture<Contracts.BoundPushRes> packet : packets) {
                require(target.nodeRid().equals(packet.get(8, TimeUnit.SECONDS).nodeRid()),
                    "ST-F3 packet did not replay on target");
            }
            assertMarkerOrder(actorId, "bound_push", List.of("S1", "S2", "S3", "S4"));
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    private void messageFollowDuration() throws Exception {
        HandoffFixture fixture = completeSimpleTransfer("ST-F4");
        sendAtRef(
            fixture.sourceHttp(),
            fixture.actorId(),
            fixture.sourceRef(),
            "ST-F4",
            "within-message-follow-duration");
        waitFor(fixture.actorId(), "message_follow_send", Duration.ofSeconds(5));
        Thread.sleep(2_500);
        require(probeAtRefFails(
                fixture.sourceHttp(), fixture.actorId(), fixture.sourceRef(), "ST-F4"),
            "ST-F4 stale ref did not fail after the Message Follow duration");
        List<Contracts.Evidence> observed = evidence();
        require(hasKind(observed, fixture.actorId(), "message_follow_relay"),
            "ST-F4 source Message Follow evidence is missing");
        require(observed.stream().anyMatch(entry ->
                fixture.actorId().equals(entry.actorId())
                    && "message_follow_rejected".equals(entry.kind())
                    && "ACTOR_LOCATION_STALE".equals(entry.value())),
            "ST-F4 stale ref was not classified as ACTOR_LOCATION_STALE");
    }

    private void messageFollowRouteRemoval() throws Exception {
        String actorId = id("actor-st-f5");
        String spotB = id("spot-st-f5-b");
        String spotC = id("spot-st-f5-c");
        createSpot(nodeB, spotB, "accept");
        createSpot(nodeC, spotC, "accept");
        createActor(nodeA, actorId, Contracts.STATEFUL, 95);
        JsonNode sourceARef = get(nodeA + "/actors/" + actorId + "/ref", JsonNode.class);
        require(join(nodeA, actorId, "ST-F5", spotB, "accept").accepted(),
            "ST-F5 first transfer failed");
        JsonNode sourceBRef = get(nodeB + "/actors/" + actorId + "/ref", JsonNode.class);
        require(join(nodeB, actorId, "ST-F5", spotC, "accept").accepted(),
            "ST-F5 chained transfer failed");
        sendAtRef(
            nodeA,
            actorId,
            sourceARef,
            "ST-F5",
            "chained-message-follow");
        waitFor(actorId, "message_follow_send", Duration.ofSeconds(5));
        Contracts.ProbeRes finalOwner = probe(nodeC, actorId, "ST-F5", "final-owner");
        require("actor-c".equals(finalOwner.nodeRid()),
            "ST-F5 chained Message Follow did not reach the final target");
        Thread.sleep(2_500);
        require(probeAtRefFails(nodeA, actorId, sourceARef, "ST-F5"),
            "ST-F5 node A Message Follow route was retained");
        require(probeAtRefFails(nodeB, actorId, sourceBRef, "ST-F5"),
            "ST-F5 node B Message Follow route was retained");
        require(evidence().stream().filter(entry ->
                actorId.equals(entry.actorId()) && "message_follow_route_removed".equals(entry.kind())).count() >= 2,
            "ST-F5 did not observe both Message Follow route removals");
    }

    private void inFlightRequestReplyCorrelationAndTimeout() throws Exception {
        HandoffFixture success = startHandoff("ST-F6");
        CompletableFuture<Contracts.ProbeRes> correlated = probeAsync(
            success.targetHttp(), success.actorId(), "ST-F6", "correlated-reply");
        Thread.sleep(200);
        require(!correlated.isDone(), "ST-F6 request completed before target replay");
        release(success.targetHttp(), success.spotRid());
        require(success.join().get(8, TimeUnit.SECONDS).accepted(),
            "ST-F6 correlation transfer failed");
        Contracts.ProbeRes reply = correlated.get(8, TimeUnit.SECONDS);
        require(success.targetNodeRid().equals(reply.nodeRid())
                && "correlated-reply".equals(reply.marker()),
            "ST-F6 reply was not correlated from the target");

        HandoffFixture timeout = startHandoff("ST-F6");
        CompletableFuture<Contracts.ProbeRes> expiring = postAsync(
            timeout.targetHttp() + "/actors/" + timeout.actorId() + "/probe-timeout",
            new Contracts.ProbeWithTimeoutReq("ST-F6", "late-reply", 250),
            Contracts.ProbeRes.class);
        try {
            expiring.get(3, TimeUnit.SECONDS);
            throw new AssertionError("ST-F6 request did not use its original timeout");
        } catch (java.util.concurrent.ExecutionException expected) {
        }
        waitFor(timeout.actorId(), "request_timeout", Duration.ofSeconds(3));
        release(timeout.targetHttp(), timeout.spotRid());
        require(timeout.join().get(8, TimeUnit.SECONDS).accepted(),
            "ST-F6 timeout transfer failed");
        waitFor(timeout.actorId(), "packet_handler", Duration.ofSeconds(5));
        Thread.sleep(300);
        long lateHandlers = evidence().stream().filter(entry ->
            timeout.actorId().equals(entry.actorId())
                && "packet_handler".equals(entry.kind())
                && "late-reply".equals(entry.value())).count();
        require(lateHandlers == 1,
            "ST-F6 late request was lost or dispatched more than once: " + lateHandlers);
    }

    private HandoffFixture startHandoff(String scenario) throws Exception {
        RemoteActorFixture remote = createRemoteActor(
            "handoff-" + scenario.toLowerCase(),
            Contracts.STATEFUL,
            90,
            "delay-joined",
            null);
        JsonNode sourceRef = get(
            remote.sourceHttp() + "/actors/" + remote.actorId() + "/ref", JsonNode.class);
        CompletableFuture<Contracts.JoinTargetRes> join = joinAsync(
            remote.sourceHttp(), remote.actorId(), scenario, remote.spotRid(), "accept");
        waitFor(remote.actorId(), "joined_wait", Duration.ofSeconds(8));
        return new HandoffFixture(
            remote.actorId(),
            remote.spotRid(),
            remote.sourceNodeRid(),
            remote.targetNodeRid(),
            remote.sourceHttp(),
            remote.targetHttp(),
            sourceRef,
            join);
    }

    private HandoffFixture completeSimpleTransfer(String scenario) throws Exception {
        HandoffFixture fixture = startHandoff(scenario);
        release(fixture.targetHttp(), fixture.spotRid());
        require(fixture.join().get(8, TimeUnit.SECONDS).accepted(), scenario + " transfer failed");
        return fixture;
    }

    private void remoteNoAdapter() throws Exception {
        RemoteActorFixture fixture = createRemoteActor(
            "no-adapter", Contracts.NO_ADAPTER, 0, "accept", null);
        require(join(fixture.sourceHttp(), fixture.actorId(), "ST-B3", fixture.spotRid(), "accept").accepted(),
            "ST-B3 default empty transfer failed");
        Contracts.ProbeRes probe = probe(
            fixture.targetHttp(), fixture.actorId(), "ST-B3", "after-default-empty");
        require(probe.stateVersion() == 0, "ST-B3 factory target state is not empty");
        require(hasKind(evidence(), fixture.actorId(), "transfer_out_empty_default"),
            "ST-B3 source default-empty evidence is missing");
        require(hasKind(evidence(), fixture.actorId(), "transfer_in_empty_default"),
            "ST-B3 target factory evidence is missing");
    }

    private void remoteEmptyState() throws Exception {
        RemoteActorFixture fixture = createRemoteActor(
            "empty-state", Contracts.EMPTY_STATE, 41, "accept", null);
        require(join(fixture.sourceHttp(), fixture.actorId(), "ST-B4", fixture.spotRid(), "accept").accepted(),
            "ST-B4 custom empty transfer failed");
        Contracts.ProbeRes probe = probe(
            fixture.targetHttp(), fixture.actorId(), "ST-B4", "after-empty-state");
        require(probe.stateVersion() == 41, "ST-B4 domain state was not loaded");
        waitFor(fixture.actorId(), "location_committed", Duration.ofSeconds(3));
        waitFor(fixture.actorId(), "message_follow_registered", Duration.ofSeconds(3));
        assertNodeOrder(fixture.actorId(), fixture.sourceNodeRid(), List.of(
            "transfer_out_empty", "leave", "commit_request", "location_visible", "success_reply"));
        assertNodeOrder(fixture.actorId(), fixture.targetNodeRid(), List.of(
            "admission", "transfer_in_empty", "joined", "domain_state_loaded", "commit_ack"));
        assertCorrelatedTransferMarkers(fixture.actorId(), List.of(
            "commit_request", "location_committed", "message_follow_registered", "commit_ack"));
    }

    private void sourceDownBeforeCommit() throws Exception {
        String actorId = id("actor-source-down-before-commit");
        String spotRid = id("spot-source-down-before-commit");
        createSpot(nodeB, spotRid, "accept");
        createActor(nodeA, actorId, Contracts.STATEFUL, 62);
        CompletableFuture<Contracts.JoinTargetRes> join = joinAsync(
            nodeA, actorId, "ST-C1", spotRid, "accept");
        waitFor(actorId, "before_commit_gate", Duration.ofSeconds(8));
        signal("ST-C1.ready");
        waitSignal("ST-C1.killed", Duration.ofSeconds(15));
        try {
            Contracts.JoinTargetRes response = join.get(15, TimeUnit.SECONDS);
            require(!response.accepted(), "ST-C1 join succeeded after source shutdown");
        } catch (java.util.concurrent.ExecutionException expected) {
        }
        Thread.sleep(13_000);
        List<Contracts.Evidence> target = get(
            nodeB + "/evidence", Contracts.EvidenceSnapshot.class).entries();
        require(noKind(target, actorId, "transfer_in"), "ST-C1 target materialized without commit");
        require(noKind(target, actorId, "joined"), "ST-C1 target joined without commit");
    }

    private void committedTransferSurvivesSourceLoss(String scenario) throws Exception {
        String actorId = id("actor-committed-source-loss");
        String spotRid = id("spot-committed-source-loss");
        createSpot(nodeB, spotRid, "accept");
        createActor(nodeA, actorId, Contracts.STATEFUL, 72);
        require(join(nodeA, actorId, scenario, spotRid, "accept").accepted(),
            scenario + " transfer failed before source shutdown");
        JsonNode before = get(nodeB + "/actors/" + actorId + "/ref", JsonNode.class);
        long generation = before.path("generation").asLong();
        signal(scenario + ".ready");
        waitSignal(scenario + ".killed", Duration.ofSeconds(15));
        Contracts.ProbeRes probe = probe(nodeB, actorId, scenario, "after-source-loss");
        require("actor-b".equals(probe.nodeRid()), scenario + " target ownership was lost");
        JsonNode after = get(nodeB + "/actors/" + actorId + "/ref", JsonNode.class);
        require(generation == after.path("generation").asLong(),
            scenario + " target generation changed after stale source cleanup");
    }

    private void boundSessionTransfer() throws Exception {
        String actorId = id("actor-bound-transfer");
        Contracts.ActorCreateRes created =
            createActor(nodeA, actorId, Contracts.STATEFUL, 81);
        Contracts.CreateSpotRes target = null;
        String spotRid = null;
        for (int attempt = 0; attempt < 8; attempt++) {
            spotRid = id("spot-bound-transfer");
            Contracts.CreateSpotRes candidate =
                createSpot(nodeB, spotRid, "accept");
            if (!candidate.nodeRid().equals(created.nodeRid())) {
                target = candidate;
                break;
            }
        }
        require(target != null, "ST-E1 could not allocate a remote target Spot");
        ZLinkStreamConnector connector = connector(
            streamEndpoint(created.nodeRid()));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.BindSessionRes bound = connector
                .request(new Contracts.BindSessionReq("ST-E1", actorId))
                .submit(Contracts.BindSessionRes.class).toCompletableFuture().join();
            require(created.nodeRid().equals(bound.nodeRid()),
                "ST-E1 session did not bind source actor");
            assertBoundPush(
                connector, actorId, "ST-E1", "before-transfer",
                created.nodeRid());
            require(join(
                    httpEndpoint(created.nodeRid()),
                    actorId,
                    "ST-E1",
                    spotRid,
                    "accept").accepted(),
                "ST-E1 remote transfer failed");
            assertBoundPush(
                connector, actorId, "ST-E1", "after-transfer",
                target.nodeRid());
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    private String streamEndpoint(String nodeRid) {
        return switch (nodeRid) {
            case "actor-a" -> options.streamAEndpoint();
            case "actor-b" -> options.streamBEndpoint();
            case "actor-c" -> options.streamCEndpoint();
            default -> throw new IllegalArgumentException(
                "unknown Actor owner: " + nodeRid);
        };
    }

    private String httpEndpoint(String nodeRid) {
        return switch (nodeRid) {
            case "actor-a" -> nodeA;
            case "actor-b" -> nodeB;
            case "actor-c" -> nodeC;
            default -> throw new IllegalArgumentException(
                "unknown Actor owner: " + nodeRid);
        };
    }

    private void failedTransferKeepsBoundSession() throws Exception {
        String actorId = id("actor-bound-failed-transfer");
        Contracts.ActorCreateRes created =
            createActor(nodeA, actorId, Contracts.FAIL_OUT, 82);
        Contracts.CreateSpotRes target = null;
        String spotRid = null;
        for (int attempt = 0; attempt < 8; attempt++) {
            spotRid = id("spot-bound-failed-transfer");
            Contracts.CreateSpotRes candidate =
                createSpot(nodeB, spotRid, "accept");
            if (!candidate.nodeRid().equals(created.nodeRid())) {
                target = candidate;
                break;
            }
        }
        require(target != null, "ST-E2 could not allocate a remote target Spot");
        ZLinkStreamConnector connector = connector(
            streamEndpoint(created.nodeRid()));
        try {
            connector.connect().submit().toCompletableFuture().join();
            connector.request(new Contracts.BindSessionReq("ST-E2", actorId))
                .submit(Contracts.BindSessionRes.class).toCompletableFuture().join();
            require(!join(
                    httpEndpoint(created.nodeRid()),
                    actorId,
                    "ST-E2",
                    spotRid,
                    "accept").accepted(),
                "ST-E2 injected transfer failure returned success");
            assertBoundPush(
                connector,
                actorId,
                "ST-E2",
                "after-failed-transfer",
                created.nodeRid());
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    private void assertBoundPush(
        ZLinkStreamConnector connector,
        String actorId,
        String scenario,
        String marker,
        String expectedNode) throws Exception {
        var waiting = connector.waitFor(Contracts.BoundPushNotify.class)
            .timeout(Duration.ofSeconds(8))
            .submit(Contracts.BoundPushNotify.class);
        Contracts.BoundPushRes reply = connector
            .request(new Contracts.BoundPushReq(scenario, marker))
            .metadata("actor-id", actorId)
            .submit(Contracts.BoundPushRes.class).toCompletableFuture().join();
        Contracts.BoundPushNotify notify = waiting.toCompletableFuture().join().payload();
        require(expectedNode.equals(reply.nodeRid()), "bound push reply used the wrong node");
        require(expectedNode.equals(notify.nodeRid()), "bound push notify used the wrong node");
        require(marker.equals(notify.marker()), "bound push notify marker mismatch");
    }

    private static ZLinkStreamConnector connector(String endpoint) {
        return ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(10),
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            Integer.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            false,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            ZLinkStreamPacketNameResolver.defaultResolver(),
            null));
    }

    private void callbackFailures() throws Exception {
        failureCase("actor-fail-out", Contracts.FAIL_OUT, "transfer_out_failed");
        failureCase("actor-fail-leave", Contracts.FAIL_LEAVE, "leave_failed");
        failureCase("actor-fail-transfer-in", Contracts.FAIL_IN, "transfer_in_failed");

        String actorId = id("actor-fail-joined");
        String spotRid = id("spot-fail-joined");
        createSpot(nodeB, spotRid, "fail-joined");
        createActor(nodeA, actorId, Contracts.STATEFUL, 54);
        require(!join(nodeA, actorId, "ST-C3", spotRid, "accept").accepted(),
            "ST-C3 joined failure returned success");
        require(hasKind(evidence(), actorId, "joined_failed"),
            "ST-C3 joined failure evidence is missing");
    }

    private void failureCase(String prefix, String actorType, String expectedKind) throws Exception {
        String actorId = id(prefix);
        String spotRid = id("spot-c3");
        createSpot(nodeB, spotRid, "accept");
        createActor(nodeA, actorId, actorType, 51);
        require(!join(nodeA, actorId, "ST-C3", spotRid, "accept").accepted(),
            "ST-C3 failure returned success: " + expectedKind);
        require(hasKind(evidence(), actorId, expectedKind),
            "ST-C3 expected evidence is missing: " + expectedKind);
    }

    private void locationCommitTiming() throws Exception {
        String actorId = id("actor-location-timing");
        String spotRid = id("spot-location-timing");
        createSpot(nodeB, spotRid, "delay-joined");
        createActor(nodeA, actorId, Contracts.STATEFUL, 61);
        CompletableFuture<Contracts.JoinTargetRes> join = joinAsync(
            nodeA, actorId, "ST-D1", spotRid, "accept");
        waitFor(actorId, "joined_wait", Duration.ofSeconds(8));
        JsonNode pending = get(nodeB + "/actors/" + actorId + "/ref", JsonNode.class);
        require(!"actor-b".equals(pending.path("nodeRid").asText()),
            "ST-D1 target location committed before joined callback");
        release(nodeB, spotRid);
        require(join.get(8, TimeUnit.SECONDS).accepted(), "ST-D1 join failed");
        JsonNode committed = get(nodeB + "/actors/" + actorId + "/ref", JsonNode.class);
        require("actor-b".equals(committed.path("nodeRid").asText()),
            "ST-D1 target location was not committed after joined callback");
    }

    private Contracts.CreateSpotRes createSpot(
        String node,
        String spotRid,
        String mode) throws Exception {
        return post(
            node + "/spots",
            new Contracts.CreateSpotReq(spotRid, mode),
            Contracts.CreateSpotRes.class);
    }

    private Contracts.ActorCreateRes createActor(
        String node,
        String actorId,
        String actorType,
        int state) throws Exception {
        return post(node + "/actors", new Contracts.ActorCreateReq(actorId, actorType, state),
            Contracts.ActorCreateRes.class);
    }

    private Contracts.JoinTargetRes join(
        String node,
        String actorId,
        String scenario,
        String spotRid,
        String mode) throws Exception {
        return joinAsync(node, actorId, scenario, spotRid, mode).get(15, TimeUnit.SECONDS);
    }

    private CompletableFuture<Contracts.JoinTargetRes> joinAsync(
        String node,
        String actorId,
        String scenario,
        String spotRid,
        String mode) throws Exception {
        return postAsync(
            node + "/actors/" + actorId + "/join",
            new Contracts.JoinTargetReq(scenario, spotRid, mode),
            Contracts.JoinTargetRes.class);
    }

    private Contracts.ProbeRes probe(
        String node,
        String actorId,
        String scenario,
        String marker) throws Exception {
        return probeAsync(node, actorId, scenario, marker).get(12, TimeUnit.SECONDS);
    }

    private CompletableFuture<Contracts.ProbeRes> probeAsync(
        String node,
        String actorId,
        String scenario,
        String marker) throws Exception {
        return postAsync(
            node + "/actors/" + actorId + "/probe",
            new Contracts.ProbeReq(scenario, marker),
            Contracts.ProbeRes.class);
    }

    private Contracts.ProbeRes probeAtRef(
        String node,
        String actorId,
        JsonNode actorRef,
        String scenario,
        String marker) throws Exception {
        return post(
            node + "/actors/" + actorId + "/probe-ref",
            new Contracts.ProbeAtRefReq(
                actorRef.path("nodeRid").asText(),
                actorRef.path("generation").asLong(),
                new Contracts.ProbeReq(scenario, marker)),
            Contracts.ProbeRes.class);
    }

    private boolean probeAtRefFails(
        String node,
        String actorId,
        JsonNode actorRef,
        String scenario) {
        try {
            probeAtRef(
                node, actorId, actorRef, scenario, "after-message-follow-duration");
            return false;
        } catch (Exception expected) {
            return true;
        }
    }

    private void sendAtRef(
        String node,
        String actorId,
        JsonNode actorRef,
        String scenario,
        String marker) throws Exception {
        post(
            node + "/actors/" + actorId + "/send-ref",
            new Contracts.SendAtRefReq(
                actorRef.path("nodeRid").asText(),
                actorRef.path("generation").asLong(),
                new Contracts.MessageFollowSendReq(scenario, marker)),
            JsonNode.class);
    }

    private boolean sendAtRefFails(
        String node,
        String actorId,
        JsonNode actorRef,
        String scenario) {
        try {
            sendAtRef(
                node, actorId, actorRef, scenario, "after-message-follow-duration");
            return false;
        } catch (Exception expected) {
            return true;
        }
    }

    private void release(String node, String key) throws Exception {
        post(node + "/gates/" + key, java.util.Map.of(), Contracts.GateReleaseRes.class);
    }

    private List<Contracts.Evidence> evidence() throws Exception {
        List<Contracts.Evidence> all = new ArrayList<>();
        all.addAll(get(nodeA + "/evidence", Contracts.EvidenceSnapshot.class).entries());
        all.addAll(get(nodeB + "/evidence", Contracts.EvidenceSnapshot.class).entries());
        all.addAll(get(nodeC + "/evidence", Contracts.EvidenceSnapshot.class).entries());
        return all;
    }

    private void waitFor(String actorId, String kind, Duration timeout) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (System.nanoTime() < deadline) {
            if (hasKind(evidence(), actorId, kind)) {
                return;
            }
            Thread.sleep(50);
        }
        throw new AssertionError("timed out waiting for " + kind + " actor=" + actorId);
    }

    private void assertNodeOrder(String actorId, String nodeRid, List<String> kinds) throws Exception {
        List<String> observed = evidence().stream()
            .filter(entry -> actorId.equals(entry.actorId()) && nodeRid.equals(entry.nodeRid()))
            .map(Contracts.Evidence::kind)
            .toList();
        int cursor = -1;
        for (String kind : kinds) {
            int next = observed.subList(cursor + 1, observed.size()).indexOf(kind);
            require(next >= 0, "missing evidence kind " + kind + " in " + observed);
            cursor += next + 1;
        }
    }

    private void assertMarkerOrder(
        String actorId,
        String kind,
        List<String> markers) throws Exception {
        List<String> observed = evidence().stream()
            .filter(entry -> actorId.equals(entry.actorId()) && kind.equals(entry.kind()))
            .map(Contracts.Evidence::value)
            .toList();
        require(observed.equals(markers),
            "marker order mismatch expected=" + markers + " actual=" + observed);
    }

    private void assertCorrelatedTransferMarkers(String actorId, List<String> kinds)
        throws Exception {
        List<Contracts.Evidence> actorEvidence = evidence().stream()
            .filter(entry -> actorId.equals(entry.actorId()))
            .toList();
        String transferId = actorEvidence.stream()
            .map(Contracts.Evidence::transferId)
            .filter(value -> !value.isBlank())
            .findFirst()
            .orElseThrow(() -> new AssertionError(
                "missing transfer id evidence actor=" + actorId));
        for (String kind : kinds) {
            Contracts.Evidence marker = actorEvidence.stream()
                .filter(entry -> kind.equals(entry.kind()))
                .findFirst()
                .orElseThrow(() -> new AssertionError(
                    "missing correlated marker " + kind + " actor=" + actorId));
            require(transferId.equals(marker.transferId()),
                "transfer id mismatch kind=" + kind + " expected=" + transferId
                    + " actual=" + marker.transferId());
            if (!"commit_ack".equals(kind)) {
                require(!marker.flowId().isBlank(), "missing flow id kind=" + kind);
                require(!marker.correlationId().isBlank(),
                    "missing message correlation id kind=" + kind);
            }
        }
    }

    private record RemoteActorFixture(
        String actorId,
        String spotRid,
        String sourceNodeRid,
        String targetNodeRid,
        String sourceHttp,
        String targetHttp) {
    }

    private record HandoffFixture(
        String actorId,
        String spotRid,
        String sourceNodeRid,
        String targetNodeRid,
        String sourceHttp,
        String targetHttp,
        JsonNode sourceRef,
        CompletableFuture<Contracts.JoinTargetRes> join) {
    }

    private RemoteActorFixture createRemoteActor(
        String prefix,
        String actorType,
        int stateVersion,
        String spotMode,
        String requiredSourceNodeRid) throws Exception {
        Contracts.ActorCreateRes actor = null;
        String actorId = null;
        Throwable lastFailure = null;
        for (int attempt = 0; attempt < 40; attempt++) {
            actorId = id(prefix + "-actor");
            try {
                Contracts.ActorCreateRes candidate =
                    createActor(nodeA, actorId, actorType, stateVersion);
                if (requiredSourceNodeRid == null
                    || requiredSourceNodeRid.equals(candidate.nodeRid())) {
                    actor = candidate;
                    break;
                }
            } catch (Exception failure) {
                lastFailure = failure;
            }
            Thread.sleep(100);
        }
        if (actor == null) {
            throw new AssertionError(
                prefix + " could not allocate the requested source Actor owner",
                lastFailure);
        }

        Contracts.CreateSpotRes target = null;
        String spotRid = null;
        for (int attempt = 0; attempt < 12; attempt++) {
            spotRid = id(prefix + "-spot");
            try {
                Contracts.CreateSpotRes candidate =
                    createSpot(nodeB, spotRid, spotMode);
                if (!candidate.nodeRid().equals(actor.nodeRid())) {
                    target = candidate;
                    break;
                }
            } catch (Exception failure) {
                lastFailure = failure;
            }
            Thread.sleep(100);
        }
        if (target == null) {
            throw new AssertionError(
                prefix + " could not allocate a remote target Spot",
                lastFailure);
        }
        return new RemoteActorFixture(
            actorId,
            spotRid,
            actor.nodeRid(),
            target.nodeRid(),
            httpEndpoint(actor.nodeRid()),
            httpEndpoint(target.nodeRid()));
    }

    private void signal(String name) throws Exception {
        Files.writeString(signals.resolve(name), "ready");
    }

    private void waitSignal(String name, Duration timeout) throws Exception {
        Path path = signals.resolve(name);
        long deadline = System.nanoTime() + timeout.toNanos();
        while (System.nanoTime() < deadline) {
            if (Files.exists(path)) {
                return;
            }
            Thread.sleep(50);
        }
        throw new AssertionError("timed out waiting for runner signal " + name);
    }

    private static boolean hasKind(List<Contracts.Evidence> entries, String actorId, String kind) {
        return entries.stream().anyMatch(entry ->
            actorId.equals(entry.actorId()) && kind.equals(entry.kind()));
    }

    private static boolean noKind(List<Contracts.Evidence> entries, String actorId, String kind) {
        return !hasKind(entries, actorId, kind);
    }

    private <T> T post(String uri, Object body, Class<T> type) throws Exception {
        return postAsync(uri, body, type).get(15, TimeUnit.SECONDS);
    }

    private <T> CompletableFuture<T> postAsync(String uri, Object body, Class<T> type)
        throws Exception {
        return postAsyncWithTimeout(
            uri, body, type, Duration.ofSeconds(14));
    }

    private <T> CompletableFuture<T> postAsyncWithTimeout(
        String uri,
        Object body,
        Class<T> type,
        Duration timeout) throws Exception {
        RequestTarget target = requestTarget(uri);
        return target.client().post(target.path())
            .timeout(timeout)
            .body(body)
            .submit(type)
            .thenApply(response -> response.body())
            .toCompletableFuture();
    }

    private <T> T get(String uri, Class<T> type) throws Exception {
        RequestTarget target = requestTarget(uri);
        return target.client().get(target.path())
            .timeout(Duration.ofSeconds(5))
            .submit(type).toCompletableFuture().join().body();
    }

    private RequestTarget requestTarget(String uri) {
        URI target = URI.create(uri);
        String baseUrl = target.getScheme() + "://" + target.getRawAuthority();
        String path = target.getRawPath();
        if (target.getRawQuery() != null) {
            path += "?" + target.getRawQuery();
        }
        if (nodeA.equals(baseUrl)) {
            return new RequestTarget(nodeAHttp, path);
        }
        if (nodeB.equals(baseUrl)) {
            return new RequestTarget(nodeBHttp, path);
        }
        if (nodeC.equals(baseUrl)) {
            return new RequestTarget(nodeCHttp, path);
        }
        throw new IllegalArgumentException("unknown SpotActorTransfer HTTP endpoint: " + baseUrl);
    }

    private static ZLinkHttpClient createHttpClient(String baseUrl) {
        return ZLinkHttpClient.create(baseUrl)
            .timeout(Duration.ofSeconds(3))
            .build();
    }

    @Override
    public void close() {
        nodeCHttp.close();
        nodeBHttp.close();
        nodeAHttp.close();
    }

    private record RequestTarget(ZLinkHttpClient client, String path) { }

    private static String id(String prefix) {
        return prefix + "-" + UUID.randomUUID().toString().replace("-", "");
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }
}
