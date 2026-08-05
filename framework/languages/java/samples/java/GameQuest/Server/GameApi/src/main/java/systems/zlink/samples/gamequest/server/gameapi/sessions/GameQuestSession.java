package systems.zlink.samples.gamequest.server.gameapi.sessions;

import java.time.Instant;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
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
    public java.util.concurrent.CompletionStage<Void> onConnected() {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDisconnected() {
        String disconnectedPlayerId = playerId;
        ZLinkSessionActor disconnectedActor = playerActor;
        java.util.concurrent.CompletionStage<Void> actorDisconnected = disconnectedActor == null
            ? java.util.concurrent.CompletableFuture.completedFuture(null)
            : disconnectedActor.notifyDisconnected();
        return actorDisconnected.thenRun(() -> {
            if (disconnectedPlayerId != null) {
                store.unbind(disconnectedPlayerId);
            }
        });
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onError(ZLinkStreamError error) {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return switch (dispatch.packetName()) {
            case "JoinSessionReq" -> handleJoin(payload.decode(Messages.JoinSessionReq.class));
            case "GetQuestProgressReq" -> handleGetProgress(payload.decode(Messages.GetQuestProgressReq.class));
            case "SyncQuestProgressReq" -> handleSync(payload.decode(Messages.SyncQuestProgressReq.class));
            case "KillMonsterReq" -> handleKill(payload.decode(Messages.KillMonsterReq.class));
            case "CollectItemReq" -> handleCollect(payload.decode(Messages.CollectItemReq.class));
            case "CompleteMissionReq" -> handleMission(payload.decode(Messages.CompleteMissionReq.class));
            case "EnterAreaReq" -> handleArea(payload.decode(Messages.EnterAreaReq.class));
            case "UnlockFeatureReq" -> handleFeature(payload.decode(Messages.UnlockFeatureReq.class));
            default -> throw new IllegalStateException("Unknown GameQuest packet: " + dispatch.packetName());
        };
    }

    private java.util.concurrent.CompletionStage<Void> handleJoin(Messages.JoinSessionReq request) {
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

    private java.util.concurrent.CompletionStage<Void> handleGetProgress(Messages.GetQuestProgressReq request) {
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

    private java.util.concurrent.CompletionStage<Void> handleSync(Messages.SyncQuestProgressReq request) {
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

    private java.util.concurrent.CompletionStage<Void> handleKill(Messages.KillMonsterReq request) {
        Messages.GameplayMsg event = event(
            request.playerId(),
            request.idempotencyKey(),
            "kill",
            request.monsterId(),
            1,
            true);
        store.recordGameplay(event);
        return process(event).thenAccept(ignored ->
            context.client().reply(new Messages.KillMonsterRes(event.eventId())).submit());
    }

    private java.util.concurrent.CompletionStage<Void> handleCollect(Messages.CollectItemReq request) {
        Messages.GameplayMsg event = event(
            request.playerId(),
            request.idempotencyKey(),
            "collect",
            request.itemId(),
            request.count(),
            true);
        store.recordGameplay(event);
        return process(event).thenAccept(ignored ->
            context.client().reply(new Messages.CollectItemRes(event.eventId())).submit());
    }

    private java.util.concurrent.CompletionStage<Void> handleMission(Messages.CompleteMissionReq request) {
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

    private java.util.concurrent.CompletionStage<Void> handleArea(Messages.EnterAreaReq request) {
        Messages.GameplayMsg event = event(
            request.playerId(),
            request.idempotencyKey(),
            "area",
            request.areaId(),
            1,
            true);
        store.recordGameplay(event);
        return process(event).thenAccept(ignored ->
            context.client().reply(new Messages.EnterAreaRes(event.eventId())).submit());
    }

    private java.util.concurrent.CompletionStage<Void> handleFeature(Messages.UnlockFeatureReq request) {
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

    private java.util.concurrent.CompletionStage<Void> process(
        Messages.GameplayMsg event) {
        return channels.sendToSpot(event.playerId(), event)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestSpotDiscovery)
            .submit();
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
