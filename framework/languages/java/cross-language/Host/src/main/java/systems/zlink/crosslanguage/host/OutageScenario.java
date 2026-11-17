package systems.zlink.crosslanguage.host;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.*;

/** File-driven orchestration runs outside application handler turns. */
final class OutageScenario {
    private final HostArgs args;
    private final EventSink sink;
    private final CrossLanguageOutageStore store;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient client;
    private final ZLinkFrameworkRuntime runtime;
    private final String actorId;
    private final String runId;
    private final boolean target;
    private ZLinkAuthoritySnapshot original;
    private RelocationActor instance;
    private int creations;
    private long newLease;
    private CompletionStage<UserSpotJoinRes> join;

    OutageScenario(HostArgs args, EventSink sink, CrossLanguageOutageStore store,
            ZLinkActorManager actors, ZLinkActorClient client, ZLinkFrameworkRuntime runtime) {
        this.args = args;
        this.sink = sink;
        this.store = store;
        this.actors = actors;
        this.client = client;
        this.runtime = runtime;
        actorId = args.option("actor-id", "cross-lang-user-spot-actor");
        runId = args.require("outage-run-id");
        target = "user-spot-target".equals(args.mode());
    }

    void run() {
        long sequence = 0;
        Path commandFile = Path.of(args.require("outage-control-prefix") + ".command");
        try {
            while (true) {
                long previous = sequence;
                String[] command = waitCommand(commandFile, previous);
                sequence = Long.parseLong(command[1]);
                execute(command[2]);
                sink.append("outage-ack|run=" + runId + "|seq=" + sequence + "|phase=" + command[2]);
                if ("completed".equals(command[2])) return;
            }
        } catch (Exception failure) {
            Throwable cause = failure;
            while ((cause instanceof ExecutionException || cause instanceof java.util.concurrent.CompletionException)
                    && cause.getCause() != null) cause = cause.getCause();
            String message = cause.getMessage();
            String detail = message != null && (message.startsWith("deadline-exceeded|")
                || message.startsWith("discarded|")) ? message : cause.toString();
            sink.append("outage-error|run=" + runId + "|seq=" + sequence + "|error=" + detail);
        }
    }

    private String[] waitCommand(Path file, long previous) throws Exception {
        while (true) {
            store.checkTiming();
            if (Files.exists(file)) {
                String[] fields = Files.readString(file).strip().split("\t", -1);
                if (fields.length == 3 && runId.equals(fields[0])) {
                    long sequence = Long.parseLong(fields[1]);
                    if (sequence == previous + 1) return fields;
                    require(sequence <= previous, "command sequence skipped");
                }
            }
            Thread.sleep(10);
        }
    }

    private void execute(String command) throws Exception {
        switch (command) {
            case "steady" -> {
                original = authority();
                require(original.steadyReady(), "actor authority is not steady Ready");
                store.sourceOwner(original.ownerId());
                if (!target) {
                    probe("steady");
                    instance = RelocationActor.observed(actorId);
                    require(instance != null, "source instance missing");
                    creations = RelocationActor.creations(actorId);
                    sink.append("outage-identity|instance=" + instance.instanceId() + "|creations=" + creations
                        + "|objectGeneration=" + original.objectGeneration()
                        + "|authorityOwnerGeneration=" + original.authorityOwnerGeneration()
                        + "|lease=" + original.ownerLeaseGeneration());
                }
            }
            case "outage" -> {
                require(!target, "outage belongs to source");
                store.outage();
                await(store.failedOperation());
                var field = ZLinkFrameworkRuntime.class.getDeclaredField("locationRuntime");
                field.setAccessible(true);
                var location = (systems.zlink.framework.runtime.locations.ZLinkLocationRuntime) field.get(runtime);
                waitUntil(() -> location.ownerLeaseSnapshot().outageCutoffNanos() != null,
                    "outage snapshot was not recorded");
                var snapshot = location.ownerLeaseSnapshot();
                require(snapshot.lastSuccessfulRequestStartedNanos() != null, "last successful lease request missing");
                store.timing(sink::append, snapshot.outageCutoffNanos() - OutageLeaseProfile.lifetimeNanos(),
                    snapshot.outageCutoffNanos());
            }
            case "expired" -> {
                require(original != null, "steady observation missing");
                waitUntil(() -> {
                    try {
                        return await(store.observer().readOwnerLease(original.ownerId())) instanceof ZLinkOwnerLeaseMissing;
                    } catch (Exception failure) { throw new IllegalStateException(failure); }
                }, "original owner lease did not expire in Redis");
            }
            case "recover" -> {
                require(!target, "recovery belongs to source");
                store.recover();
                waitUntil(() -> {
                    try {
                        var read = await(store.observer().readOwnerLease(original.ownerId()));
                        if (read instanceof ZLinkOwnerLeaseFound found
                                && found.token().leaseGeneration() != original.ownerLeaseGeneration()) {
                            newLease = found.token().leaseGeneration();
                            return true;
                        }
                        return false;
                    } catch (Exception failure) { throw new IllegalStateException(failure); }
                }, "new owner lease was not claimed");
                store.newClaim();
                sink.append("outage-recovered-lease|old=" + original.ownerLeaseGeneration() + "|new=" + newLease);
            }
            case "release-descriptor" -> await(store.releaseDescriptor());
            case "release-cas" -> await(store.releaseCas());
            case "partial" -> {
                await(store.bothConfirmationsSeen());
                int before = instance.probeExecutions();
                try {
                    probe("partial");
                    throw new IllegalStateException("operation ran before both confirmations");
                } catch (ExecutionException failure) {
                    Throwable cause = failure.getCause();
                    while (cause instanceof java.util.concurrent.CompletionException) cause = cause.getCause();
                    require(cause instanceof ZLinkFrameworkException framework
                        && framework.kind() == ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                        "partial confirmation operation did not fail with ShuttingDown: " + cause);
                }
                require(before == instance.probeExecutions(), "partial confirmation executed a handler");
                sink.append("outage-partial|kind=ShuttingDown|handlerExecutions=0");
            }
            case "resumed" -> {
                store.assertConfirmed();
                probe("resumed");
                require(instance == RelocationActor.observed(actorId), "source actor instance changed");
                require(creations == RelocationActor.creations(actorId), "source actor was recreated");
                ZLinkAuthoritySnapshot recovered = authority();
                require(original.sameIncarnation(recovered), "source authority incarnation changed");
                require(recovered.ownerLeaseGeneration() == newLease, "Preserve did not use the recovered lease");
                require(store.descriptorLease() == newLease && store.preserveLease() == newLease,
                    "descriptor and Preserve confirmations used different leases");
                sink.append("outage-resumed|instance=" + instance.instanceId() + "|creations=" + creations);
            }
            case "relocation" -> join = client.requestToActor(actorId,
                new BeginUserSpotJoinReq(args.require("spot-id"), "outage-relocation"))
                .timeout(Duration.ofSeconds(45)).submit(UserSpotJoinRes.class);
            case "staged" -> {
                await(store.commitSeen());
                var found = await(actors.find(actorId)).orElseThrow();
                require(found.nodeRid().equals(original.allocation().descriptor().rid()),
                    "public Find exposed the target before NewOwner commit");
                RelocationActor staged = RelocationActor.observed(actorId);
                require(staged != null && staged.probeExecutions() == 0, "target handler executed before commit");
                require(stagedFenceAbsent(found.objectGeneration()), "target staged owner fence is present");
                sink.append("outage-staged|find=source|handlerExecutions=0|fence=absent");
            }
            case "commit" -> store.releaseCommit();
            case "completed" -> {
                if (join != null) require(await(join).accepted(), "public Join was rejected");
                UserSpotProbeRes first = probe("committed");
                UserSpotProbeRes next = probe("follow-up");
                require(!first.nodeRid().equals(original.allocation().descriptor().rid().toString()),
                    "post-commit reply came from source");
                require(first.nodeRid().equals(next.nodeRid()), "follow-up reply changed owner");
                // Recovered source/coordinator fence is observed only on .NET; the other sources record Preparing/Captured in authority after https://github.com/zlink-systems/zlink/issues/4718.
                sink.append("outage-completed|nodeRid=" + next.nodeRid());
            }
            default -> throw new IllegalArgumentException("unknown outage command " + command);
        }
    }

    private ZLinkAuthoritySnapshot authority() throws Exception {
        var result = await(store.observer().read(ZLinkAuthorityKeyCodec.actor(actorId), () -> false));
        require(result instanceof ZLinkAuthoritySnapshot, "actor authority missing");
        return (ZLinkAuthoritySnapshot) result;
    }

    private UserSpotProbeRes probe(String marker) throws Exception {
        return await(client.requestToActor(actorId, new UserSpotProbeReq(marker))
            .timeout(Duration.ofSeconds(5)).submit(UserSpotProbeRes.class));
    }

    private boolean stagedFenceAbsent(long objectGeneration) throws Exception {
        var actorsMethod = ZLinkFrameworkRuntime.class.getDeclaredMethod("actors");
        actorsMethod.setAccessible(true);
        ZLinkActorRuntime actorRuntime = (ZLinkActorRuntime) actorsMethod.invoke(runtime);
        var nodeField = ZLinkActorRuntime.class.getDeclaredField("spotNode");
        nodeField.setAccessible(true);
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) nodeField.get(actorRuntime);
        var actor = new ZLinkBackendActorRef(RoutingId.from(args.require("node-rid")), actorId, objectGeneration);
        return node.actorAuthorityOwnerGeneration(actor) == 0 && node.actorAuthorityOwnerLeaseGeneration(actor) == 0;
    }

    private <T> T await(CompletionStage<T> stage) throws Exception {
        var future = stage.toCompletableFuture();
        long deadline = System.nanoTime() + Duration.ofSeconds(60).toNanos();
        while (true) {
            store.checkTiming();
            try { return future.get(10, TimeUnit.MILLISECONDS); }
            catch (java.util.concurrent.TimeoutException pending) {
                require(System.nanoTime() < deadline, "outage observation timed out");
            }
        }
    }

    private void waitUntil(BooleanSupplier observed, String failure) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(60).toNanos();
        while (!observed.getAsBoolean()) {
            store.checkTiming();
            if (System.nanoTime() >= deadline) throw new IllegalStateException(failure);
            Thread.sleep(10);
        }
    }

    private static void require(boolean condition, String failure) {
        if (!condition) throw new IllegalStateException(failure);
    }
}
