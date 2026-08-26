package systems.zlink.samples.gamequest.server.gameapi.sessions;
import java.util.concurrent.CompletableFuture;

import java.time.Instant;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;
import systems.zlink.samples.gamequest.server.configuration.SampleTimings;
import systems.zlink.samples.gamequest.server.configuration.SampleTopology;
import systems.zlink.samples.gamequest.server.gameapi.store.GameQuestStore;
import systems.zlink.samples.gamequest.shared.contracts.Messages;

public final class GameQuestSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkRouteClient channels;
    private final GameQuestStore store;
    private final SampleTopology topology;
    private final ZLinkActorManager actors;
    private ZLinkSessionActor playerActor;
    private String playerId;

    public GameQuestSession(
        ZLinkSessionContext context,
        ZLinkRouteClient channels,
        GameQuestStore store,
        SampleTopology topology,
        ZLinkActorManager actors) {
        this.context = context;
        this.channels = channels;
        this.store = store;
        this.topology = topology;
        this.actors = actors;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        String disconnectedPlayerId = playerId;
        ZLinkSessionActor disconnectedActor = playerActor;
        CompletionStage<Void> actorDisconnected = disconnectedActor == null
            ? CompletableFuture.completedFuture(null)
            : disconnectedActor.notifyDisconnected();
        return actorDisconnected.thenRun(() -> {
            if (disconnectedPlayerId != null) {
                store.unbind(disconnectedPlayerId);
            }
        });
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return switch (dispatch.packetName()) {
            case "JoinSessionReq" -> handleJoin(payload.decode(Messages.JoinSessionReq.class));
            case "GetQuestProgressReq" -> handleGetProgress(payload.decode(Messages.GetQuestProgressReq.class));
            case "SyncQuestProgressReq" -> handleSync(payload.decode(Messages.SyncQuestProgressReq.class));
            case "KillMonsterReq" -> handleKill(payload.decode(Messages.KillMonsterReq.class));
            case "CollectItemMsg" -> handleCollect(payload.decode(Messages.CollectItemMsg.class));
            case "CompleteMissionReq" -> handleMission(payload.decode(Messages.CompleteMissionReq.class));
            case "EnterAreaMsg" -> handleArea(payload.decode(Messages.EnterAreaMsg.class));
            case "UnlockFeatureReq" -> handleFeature(payload.decode(Messages.UnlockFeatureReq.class));
            default -> throw new IllegalStateException("Unknown GameQuest packet: " + dispatch.packetName());
        };
    }

    private CompletionStage<Void> handleJoin(Messages.JoinSessionReq request) {
        playerId = request.playerId();
        store.bind(request.playerId(), topology.gameApi().instanceName());
        return ensurePlayerActor(request)
            .thenCompose(actorRef -> context.actors().bind(actorRef))
            .thenCompose(bound -> {
                playerActor = bound;
                return channels
            .requestToSpot(
                request.playerId(),
                new Messages.GetQuestProgressReq(request.playerId()))
            .timeout(SampleTimings.RequestTimeout)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestSpotDiscovery)
            .submit(Messages.GetQuestProgressRes.class)
            .thenAccept(ownerProjection -> {
                store.mergeProjection(request.playerId(), ownerProjection.activeQuests());
                context.client().reply(new Messages.JoinSessionRes(ownerProjection.activeQuests())).submit();
            });
            });
    }

    private CompletionStage<Void> handleGetProgress(Messages.GetQuestProgressReq request) {
        return channels
            .requestToSpot(request.playerId(), request)
            .timeout(SampleTimings.RequestTimeout)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestSpotDiscovery)
            .submit(Messages.GetQuestProgressRes.class)
            .thenAccept(ownerProjection -> {
                store.mergeProjection(request.playerId(), ownerProjection.activeQuests());
                context.client().reply(ownerProjection).submit();
            });
    }

    private CompletionStage<Void> handleSync(Messages.SyncQuestProgressReq request) {
        return channels
            .requestToSpot(request.playerId(), request)
            .timeout(SampleTimings.RequestTimeout)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestSpotDiscovery)
            .submit(Messages.SyncQuestProgressRes.class)
            .thenAccept(response -> {
                store.mergeProjection(request.playerId(), response.updatedQuests());
                context.client().reply(response).submit();
            });
    }

    private CompletionStage<Void> handleKill(Messages.KillMonsterReq request) {
        Messages.GameplayMsg event = event(
            request.playerId(),
            request.idempotencyKey(),
            "kill",
            request.monsterId(),
            1,
            true);
        store.recordGameplay(event);
        return process(event)
            .thenRun(() -> System.out.printf(
                "gamequest-api event-routed player=%s%n", event.playerId()))
            .thenCompose(ignored ->
                context.client().reply(new Messages.KillMonsterRes(event.eventId())).submit())
            .exceptionallyCompose(error -> {
                if (isUnavailable(error)) {
                    System.out.printf("gamequest-owner unavailable player=%s%n", event.playerId());
                    return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "Mission owner is unavailable",
                        error));
                }
                return CompletableFuture.failedFuture(error);
            });
    }

    private CompletionStage<Void> handleCollect(Messages.CollectItemMsg message) {
        Messages.GameplayMsg event = event(
            message.playerId(),
            message.idempotencyKey(),
            "collect",
            message.itemId(),
            message.count(),
            true);
        store.recordGameplay(event);
        return process(event);
    }

    private CompletionStage<Void> handleMission(Messages.CompleteMissionReq request) {
        Messages.GameplayMsg event = event(
            request.playerId(),
            request.idempotencyKey(),
            "mission",
            request.missionId(),
            1,
            true);
        store.recordGameplay(event);
        return process(event).thenAccept(ignored ->
            context.client().reply(new Messages.CompleteMissionRes(event.eventId())).submit());
    }

    private CompletionStage<Void> handleArea(Messages.EnterAreaMsg message) {
        Messages.GameplayMsg event = event(
            message.playerId(),
            message.idempotencyKey(),
            "area",
            message.areaId(),
            1,
            true);
        store.recordGameplay(event);
        return process(event);
    }

    private CompletionStage<Void> handleFeature(Messages.UnlockFeatureReq request) {
        Messages.GameplayMsg event = event(
            request.playerId(),
            request.idempotencyKey(),
            "feature",
            request.featureId(),
            1,
            true);
        store.recordGameplay(event);
        return process(event).thenAccept(ignored ->
            context.client().reply(new Messages.UnlockFeatureRes(event.eventId())).submit());
    }

    private CompletionStage<Void> process(
        Messages.GameplayMsg event) {
        return channels.sendToSpot(event.playerId(), event)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestSpotDiscovery)
            .submit();
    }

    private static boolean isUnavailable(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZLinkFrameworkException framework
                && framework.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE) {
                return true;
            }
            if (current instanceof ZlinkSubmitException submit
                && submit.getResult() == SubmitResult.NOT_CONNECTED) {
                return true;
            }
            current = current.getCause();
        }
        return false;
    }

    private CompletionStage<ActorRef> ensurePlayerActor(Messages.JoinSessionReq request) {
        return actors.getOrCreate(request.playerId(), SampleNames.PlayerSessionActorType)
            .request(request)
            .submit()
            .thenApply(result -> {
                if (result instanceof ZLinkActorCreateResult.Existing existing) {
                    return existing.actor();
                }
                if (result instanceof ZLinkActorCreateResult.Created created) {
                    return created.actor();
                }
                throw new IllegalStateException("Player session Actor creation was rejected");
            });
    }

    private Messages.GameplayMsg event(
        String playerId,
        String idempotencyKey,
        String eventType,
        String value,
        int count,
        boolean publish) {
        return Messages.GameplayMsg.create(
            playerId + "-" + idempotencyKey,
            playerId,
            eventType,
            idempotencyKey,
            value,
            count,
            topology.gameApi().instanceName(),
            Instant.now().toEpochMilli(),
            publish);
    }
}
