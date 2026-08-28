package systems.zlink.samples.gamequest.client;

import java.io.IOException;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardWatchEventKinds;
import java.nio.file.WatchEvent;
import java.nio.file.WatchKey;
import java.nio.file.WatchService;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;
import systems.zlink.samples.gamequest.shared.contracts.Messages;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class GameQuestClientScenario {
    private final GameQuestClientOptions options;
    private final Function<String, ZLinkStreamConnector> connectorFactory;

    public GameQuestClientScenario(
        GameQuestClientOptions options,
        Function<String, ZLinkStreamConnector> connectorFactory) {
        this.options = options;
        this.connectorFactory = connectorFactory;
    }

    public void run(ZLinkStreamConnector apiAStream, ZLinkStreamConnector apiBStream) throws Exception {
        apiAStream.connect().submit().toCompletableFuture().join();
        Messages.JoinSessionRes joined = apiAStream
            .request(new Messages.JoinSessionReq("player-alice"))
            .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();
        ensure(joined.activeQuests().isEmpty());
        AtomicInteger aliceSawBobProgress = new AtomicInteger();
        apiAStream.on(Messages.QuestProgressNotify.class, message -> {
            if (message.payload().playerId().equals("player-bob")) {
                aliceSawBobProgress.incrementAndGet();
            }
            return CompletableFuture.completedFuture(null);
        });

        CompletionStage<ZLinkStreamMessage<Messages.QuestProgressNotify>> firstProgress =
            apiAStream.waitFor(Messages.QuestProgressNotify.class).submit(Messages.QuestProgressNotify.class);
        Messages.KillMonsterRes firstKill = apiAStream
            .request(new Messages.KillMonsterReq("player-alice", "wolf", "forest", "kill-1"))
            .submit(Messages.KillMonsterRes.class).toCompletableFuture().join();
        ensure(firstKill.eventId().equals("player-alice-kill-1"));
        Messages.QuestProgressNotify firstPush = firstProgress.toCompletableFuture().join().payload();
        ensure(firstPush.playerId().equals("player-alice"));
        ensure(firstPush.progress().questId().equals(Messages.QuestIds.FirstHunt));
        ensure(firstPush.progress().currentCount() == 1);

        Messages.KillMonsterRes firstDuplicate = apiAStream
            .request(new Messages.KillMonsterReq("player-alice", "wolf", "forest", "kill-1"))
            .submit(Messages.KillMonsterRes.class).toCompletableFuture().join();
        ensure(firstDuplicate.eventId().equals(firstKill.eventId()));
        Messages.GetQuestProgressRes progressAfterFirstDuplicate = apiAStream
            .request(new Messages.GetQuestProgressReq("player-alice"))
            .submit(Messages.GetQuestProgressRes.class).toCompletableFuture().join();
        ensure(hasProgress(progressAfterFirstDuplicate.activeQuests(), Messages.QuestIds.FirstHunt, 1));

        CompletionStage<ZLinkStreamMessage<Messages.QuestCompletedNotify>> firstHuntCompleted =
            apiAStream.waitFor(Messages.QuestCompletedNotify.class)
                .where(Messages.QuestCompletedNotify.class, message ->
                    message.payload().progress().questId().equals(Messages.QuestIds.FirstHunt))
                .submit(Messages.QuestCompletedNotify.class);
        apiAStream.request(new Messages.KillMonsterReq("player-alice", "wolf", "forest", "kill-2"))
            .submit(Messages.KillMonsterRes.class).toCompletableFuture().join();
        Messages.KillMonsterRes thirdKill = apiAStream
            .request(new Messages.KillMonsterReq("player-alice", "wolf", "forest", "kill-3"))
            .submit(Messages.KillMonsterRes.class).toCompletableFuture().join();
        ensure(thirdKill.eventId().equals("player-alice-kill-3"));
        Messages.QuestCompletedNotify firstHuntPush = firstHuntCompleted.toCompletableFuture().join().payload();
        ensure(firstHuntPush.rewardGranted());
        ensure(firstHuntPush.progress().status().equals(Messages.QuestStatuses.RewardGranted));

        Messages.KillMonsterRes duplicate = apiAStream
            .request(new Messages.KillMonsterReq("player-alice", "wolf", "forest", "kill-3"))
            .submit(Messages.KillMonsterRes.class).toCompletableFuture().join();
        ensure(duplicate.eventId().equals(thirdKill.eventId()));

        CompletionStage<ZLinkStreamMessage<Messages.QuestCompletedNotify>> auctionCompleted =
            apiAStream.waitFor(Messages.QuestCompletedNotify.class)
                .where(Messages.QuestCompletedNotify.class, message ->
                    message.payload().progress().questId().equals(Messages.QuestIds.OpenAuction))
                .submit(Messages.QuestCompletedNotify.class);
        Messages.UnlockFeatureRes auction = apiAStream
            .request(new Messages.UnlockFeatureReq("player-alice", "auction", "unlock-auction"))
            .submit(Messages.UnlockFeatureRes.class).toCompletableFuture().join();
        ensure(auction.eventId().equals("player-alice-unlock-auction"));
        ensure(auctionCompleted.toCompletableFuture().join().payload().rewardGranted());
        Messages.GetGameplaySnapshotRes snapshot = post(
            options.apiAHttpEndpoint(),
            "/internal/snapshot",
            new Messages.GetGameplaySnapshotReq("player-alice"),
            Messages.GetGameplaySnapshotRes.class);
        ensure(snapshot.unlockedFeatureIds().contains("auction"));

        Messages.CompleteMissionRes tutorial = apiAStream
            .request(new Messages.CompleteMissionReq("player-alice", "tutorial", "mission-tutorial"))
            .submit(Messages.CompleteMissionRes.class).toCompletableFuture().join();
        ensure(tutorial.eventId().equals("player-alice-mission-tutorial"));
        apiAStream.send(new Messages.EnterAreaMsg(
                "player-alice", "ruins", "enter-ruins"))
            .submit().toCompletableFuture().join();

        apiAStream.send(new Messages.CollectItemMsg(
                "player-bob", "healing-herb", 1, "herb-1"))
            .submit().toCompletableFuture().join();
        waitForProjection("player-bob", Messages.QuestIds.HerbGathering, 1);
        Messages.GetQuestProgressRes offlineProgress = apiAStream
            .request(new Messages.GetQuestProgressReq("player-bob"))
            .submit(Messages.GetQuestProgressRes.class).toCompletableFuture().join();
        ensure(hasProgress(offlineProgress.activeQuests(), Messages.QuestIds.HerbGathering, 1));

        apiBStream.connect().submit().toCompletableFuture().join();
        Messages.JoinSessionRes bobJoined = apiBStream
            .request(new Messages.JoinSessionReq("player-bob"))
            .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();
        ensure(hasProgress(bobJoined.activeQuests(), Messages.QuestIds.HerbGathering, 1));
        ensure(aliceSawBobProgress.get() == 0);

        CompletionStage<ZLinkStreamMessage<Messages.QuestCompletedNotify>> herbCompleted =
            apiBStream.waitFor(Messages.QuestCompletedNotify.class)
                .where(Messages.QuestCompletedNotify.class, message ->
                    message.payload().progress().questId().equals(Messages.QuestIds.HerbGathering))
                .submit(Messages.QuestCompletedNotify.class);
        apiBStream.send(new Messages.CollectItemMsg(
                "player-bob", "healing-herb", 4, "herb-2"))
            .submit().toCompletableFuture().join();
        Messages.QuestCompletedNotify herbPush = herbCompleted.toCompletableFuture().join().payload();
        ensure(herbPush.playerId().equals("player-bob"));
        ensure(herbPush.rewardGranted());
        ensure(herbPush.progress().status().equals(Messages.QuestStatuses.RewardGranted));

        ensure(postRaw(options.apiAHttpEndpoint(),
            "/self-check/projection/player-bob/" + Messages.QuestIds.HerbGathering + "/delete"));
        Messages.GetQuestProgressRes missingProjection = apiBStream
            .request(new Messages.GetQuestProgressReq("player-bob"))
            .submit(Messages.GetQuestProgressRes.class).toCompletableFuture().join();
        ensure(missingProjection.activeQuests().stream()
            .noneMatch(progress -> progress.questId().equals(Messages.QuestIds.HerbGathering)));
        Messages.QuestProgress rebuilt = post(
            options.apiAHttpEndpoint(),
            "/self-check/projection/player-bob/" + Messages.QuestIds.HerbGathering + "/rebuild",
            "",
            Messages.QuestProgress.class);
        ensure(rebuilt.questId().equals(Messages.QuestIds.HerbGathering));
        ensure(rebuilt.status().equals(Messages.QuestStatuses.RewardGranted));
        Messages.GetQuestProgressRes rebuiltProjection = apiBStream
            .request(new Messages.GetQuestProgressReq("player-bob"))
            .submit(Messages.GetQuestProgressRes.class).toCompletableFuture().join();
        ensure(rebuiltProjection.activeQuests().stream().anyMatch(progress ->
            progress.questId().equals(Messages.QuestIds.HerbGathering)
                && progress.status().equals(Messages.QuestStatuses.RewardGranted)));

        ensure(postRaw(options.apiBHttpEndpoint(), "/self-check/gameplay/kill-without-publish/player-alice"));
        ensure(postRaw(options.apiBHttpEndpoint(), "/self-check/gameplay/kill-without-publish/player-alice"));
        Messages.SyncQuestProgressRes sync = apiAStream
            .request(new Messages.SyncQuestProgressReq("player-alice"))
            .submit(Messages.SyncQuestProgressRes.class).toCompletableFuture().join();
        ensure(sync.updatedQuests().stream().anyMatch(progress ->
            progress.questId().equals(Messages.QuestIds.FirstHunt) && progress.currentCount() == 5));
        Messages.GetQuestProgressRes reconciled = apiBStream
            .request(new Messages.GetQuestProgressReq("player-alice"))
            .submit(Messages.GetQuestProgressRes.class).toCompletableFuture().join();
        ensure(reconciled.activeQuests().stream().anyMatch(progress ->
            progress.questId().equals(Messages.QuestIds.FirstHunt) && progress.currentCount() == 5));

        apiAStream.close().submit().toCompletableFuture().join();
        apiBStream.close().submit().toCompletableFuture().join();
        ZLinkStreamConnector reconnectedStream = connectorFactory.apply(options.apiBStreamEndpoint());
        try {
            reconnectedStream.connect().submit().toCompletableFuture().join();
            Messages.JoinSessionRes reconnectedAlice = reconnectedStream
                .request(new Messages.JoinSessionReq("player-alice"))
                .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();
            ensure(reconnectedAlice.activeQuests().stream().anyMatch(progress ->
                progress.questId().equals(Messages.QuestIds.FirstHunt)
                    && progress.status().equals(Messages.QuestStatuses.RewardGranted)));

            CompletionStage<ZLinkStreamMessage<Messages.QuestProgressNotify>> reconnectedProgress =
                reconnectedStream.waitFor(Messages.QuestProgressNotify.class)
                    .where(Messages.QuestProgressNotify.class, message ->
                        message.payload().playerId().equals("player-alice")
                            && message.payload().progress().questId().equals(Messages.QuestIds.HerbGathering))
                    .submit(Messages.QuestProgressNotify.class);
            reconnectedStream.send(new Messages.CollectItemMsg(
                    "player-alice", "healing-herb", 1, "reconnect-herb-1"))
                .submit().toCompletableFuture().join();
            ensure(reconnectedProgress.toCompletableFuture().join().payload().progress().currentCount() == 1);
        } finally {
            reconnectedStream.close().submit().toCompletableFuture().join();
        }

        ZLinkStreamConnector scaleA = connectorFactory.apply(options.apiAStreamEndpoint());
        ZLinkStreamConnector scaleB = connectorFactory.apply(options.apiBStreamEndpoint());
        try {
            verifyScaleOut(scaleA, scaleB);
        } finally {
            scaleA.close().submit().toCompletableFuture().join();
            scaleB.close().submit().toCompletableFuture().join();
        }
        Messages.GameQuestServerAssertRes assertion = waitForServerAssertion();
        ensure(assertion.passed());
        System.out.println(SampleNames.ServerEvidenceMarker);
    }

    public void verifyRehydrated(ZLinkStreamConnector apiAStream) {
        apiAStream.connect().submit().toCompletableFuture().join();
        Messages.JoinSessionRes joined = apiAStream
            .request(new Messages.JoinSessionReq("player-alice"))
            .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();
        ensure(joined.activeQuests().stream().anyMatch(progress ->
            progress.questId().equals(Messages.QuestIds.FirstHunt)
                && progress.currentCount() == 5
                && progress.status().equals(Messages.QuestStatuses.RewardGranted)));
        apiAStream.close().submit().toCompletableFuture().join();
    }

    public void verifyOwnerUnavailable(ZLinkStreamConnector apiAStream) throws Exception {
        String playerId = "player-owner-unavailable";
        apiAStream.connect().submit().toCompletableFuture().join();
        apiAStream.request(new Messages.JoinSessionReq(playerId))
            .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();
        waitForOwnerTerminationRelease();
        try {
            apiAStream.request(new Messages.KillMonsterReq(
                    playerId, "wolf", "forest", "owner-unavailable-kill"))
                .submit(Messages.KillMonsterRes.class).toCompletableFuture().join();
            throw new IllegalStateException("Ready owner gameplay call unexpectedly succeeded");
        } catch (java.util.concurrent.CompletionException error) {
            ensure(isUnavailable(error));
        }
        apiAStream.close().submit().toCompletableFuture().join();
    }

    private void verifyScaleOut(
        ZLinkStreamConnector apiAStream,
        ZLinkStreamConnector apiBStream) {
        apiAStream.connect().submit().toCompletableFuture().join();
        apiBStream.connect().submit().toCompletableFuture().join();
        apiAStream.request(new Messages.JoinSessionReq("player-scale-a"))
            .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();
        apiBStream.request(new Messages.JoinSessionReq("player-scale-b"))
            .submit(Messages.JoinSessionRes.class).toCompletableFuture().join();

        CompletableFuture<ZLinkStreamMessage<Messages.QuestProgressNotify>> progressA =
            apiAStream.waitFor(Messages.QuestProgressNotify.class)
                .where(Messages.QuestProgressNotify.class, message ->
                    message.payload().playerId().equals("player-scale-a"))
                .submit(Messages.QuestProgressNotify.class).toCompletableFuture();
        CompletableFuture<ZLinkStreamMessage<Messages.QuestProgressNotify>> progressB =
            apiBStream.waitFor(Messages.QuestProgressNotify.class)
                .where(Messages.QuestProgressNotify.class, message ->
                    message.payload().playerId().equals("player-scale-b"))
                .submit(Messages.QuestProgressNotify.class).toCompletableFuture();
        CompletableFuture<Messages.KillMonsterRes> requestA = apiAStream
            .request(new Messages.KillMonsterReq(
                "player-scale-a", "wolf", "forest", "scale-kill-1"))
            .submit(Messages.KillMonsterRes.class).toCompletableFuture();
        CompletableFuture<Void> requestB = apiBStream
            .send(new Messages.CollectItemMsg(
                "player-scale-b", "healing-herb", 1, "scale-herb-1"))
            .submit().toCompletableFuture();

        CompletableFuture.allOf(requestA, requestB, progressA, progressB).join();
        ensure(progressA.join().payload().progress().currentCount() == 1);
        ensure(progressB.join().payload().progress().currentCount() == 1);
    }

    private Messages.GameQuestServerAssertRes waitForServerAssertion() throws Exception {
        Instant deadline = Instant.now().plus(Duration.ofSeconds(10));
        Messages.GameQuestServerAssertRes last = null;
        while (Instant.now().isBefore(deadline)) {
            last = post(
                options.apiAHttpEndpoint(),
                "/self-check/assert",
                "",
                Messages.GameQuestServerAssertRes.class);
            if (last.passed()) {
                return last;
            }
        }
        return last;
    }

    private void waitForProjection(String playerId, String questId, int currentCount) throws Exception {
        Instant deadline = Instant.now().plus(Duration.ofSeconds(10));
        do {
            Messages.GetQuestProgressRes projection = post(
                options.apiAHttpEndpoint(),
                "/quest/progress/" + playerId,
                "",
                Messages.GetQuestProgressRes.class);
            if (hasProgress(projection.activeQuests(), questId, currentCount)) {
                return;
            }
        } while (Instant.now().isBefore(deadline));
        throw new IllegalStateException("Projection did not reach " + questId + "=" + currentCount);
    }

    private void waitForOwnerTerminationRelease() throws IOException, InterruptedException {
        if (options.ownerUnavailableReleaseFile().isBlank()) {
            throw new IllegalStateException("sample.ownerUnavailableReleaseFile is required");
        }
        Path releaseFile = Path.of(options.ownerUnavailableReleaseFile()).toAbsolutePath().normalize();
        Path parent = releaseFile.getParent();
        if (parent == null) {
            throw new IllegalStateException("Release file must have a parent directory");
        }
        if (Files.exists(releaseFile)) {
            return;
        }

        long deadlineNanos = System.nanoTime() + Duration.ofSeconds(30).toNanos();
        try (WatchService watcher = FileSystems.getDefault().newWatchService()) {
            parent.register(watcher, StandardWatchEventKinds.ENTRY_CREATE);
            if (Files.exists(releaseFile)) {
                return;
            }

            while (true) {
                long remainingNanos = deadlineNanos - System.nanoTime();
                if (remainingNanos <= 0) {
                    break;
                }
                WatchKey key = watcher.poll(remainingNanos, TimeUnit.NANOSECONDS);
                if (key == null) {
                    break;
                }
                boolean released = key.pollEvents().stream()
                    .map(WatchEvent::context)
                    .filter(Path.class::isInstance)
                    .map(Path.class::cast)
                    .anyMatch(releaseFile.getFileName()::equals);
                boolean valid = key.reset();
                if (released || Files.exists(releaseFile)) {
                    return;
                }
                if (!valid) {
                    break;
                }
            }
        }
        throw new IllegalStateException("Timed out waiting for owner termination release");
    }

    private static boolean isUnavailable(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZLinkFrameworkException framework
                && framework.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE) {
                return true;
            }
            if (current instanceof IllegalStateException remote
                && remote.getMessage() != null
                && remote.getMessage().startsWith("Unavailable:")) {
                return true;
            }
            current = current.getCause();
        }
        return false;
    }

    private boolean hasProgress(List<Messages.QuestProgress> progress, String questId, int currentCount) {
        return progress.stream()
            .anyMatch(item -> item.questId().equals(questId) && item.currentCount() == currentCount);
    }

    private boolean postRaw(String base, String path) throws Exception {
        var response = ZLinkHttpClient.create(base)
            .post(path)
            .submitRaw()
            .toCompletableFuture()
            .join();
        return response.status() >= 200 && response.status() < 300;
    }

    private <T> T post(String base, String path, Object body, Class<T> type) throws Exception {
        var request = ZLinkHttpClient.create(base).post(path);
        if (!(body instanceof String value) || !value.isEmpty()) {
            request.body(body);
        }
        return request.fetch(type).toCompletableFuture().join();
    }

    private static void ensure(boolean condition) {
        if (!condition) {
            throw new IllegalStateException("Ensure failed");
        }
    }
}
