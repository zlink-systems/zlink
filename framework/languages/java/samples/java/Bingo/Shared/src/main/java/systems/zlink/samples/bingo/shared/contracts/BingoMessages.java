package systems.zlink.samples.bingo.shared.contracts;

import java.util.List;

public final class BingoMessages {
    private BingoMessages() {
    }

    public static Messages.AuthenticateReq authenticateReq(String accessToken) {
        return Messages.AuthenticateReq.newBuilder()
            .setAccessToken(accessToken)
            .build();
    }

    public static Messages.AuthenticateRes authenticateRes(
        String actorId,
        String displayName) {
        return Messages.AuthenticateRes.newBuilder()
            .setActorId(actorId)
            .setDisplayName(displayName)
            .build();
    }

    public static Messages.AuthenticatePlayerReq authenticatePlayerReq(String accessToken) {
        return Messages.AuthenticatePlayerReq.newBuilder()
            .setAccessToken(accessToken)
            .build();
    }

    public static Messages.AuthenticatePlayerRes authenticatePlayerRes(
        boolean accepted,
        String actorId,
        String displayName,
        String reason) {
        Messages.AuthenticatePlayerRes.Builder builder = Messages.AuthenticatePlayerRes.newBuilder()
            .setAccepted(accepted)
            .setActorId(actorId == null ? "" : actorId)
            .setDisplayName(displayName == null ? "" : displayName);
        if (reason != null) {
            builder.setReason(reason);
        }
        return builder.build();
    }

    public static Messages.GetPlayerRecordReq getPlayerRecordReq(String actorId) {
        return Messages.GetPlayerRecordReq.newBuilder().setActorId(actorId).build();
    }

    public static Messages.GetPlayerRecordRes getPlayerRecordRes(
        String actorId, int wins, int losses) {
        return Messages.GetPlayerRecordRes.newBuilder()
            .setActorId(actorId).setWins(wins).setLosses(losses).build();
    }

    public static Messages.ReportBingoResultReq reportBingoResultReq(
        String roomId, String actorId, boolean won, int finalDrawSeq) {
        return Messages.ReportBingoResultReq.newBuilder()
            .setRoomId(roomId).setActorId(actorId).setWon(won)
            .setFinalDrawSeq(finalDrawSeq).build();
    }

    public static Messages.ReportBingoResultRes reportBingoResultRes(
        String actorId, int wins, int losses) {
        return Messages.ReportBingoResultRes.newBuilder()
            .setActorId(actorId).setWins(wins).setLosses(losses).build();
    }

    public static Messages.EnsurePlayerActorReq ensurePlayerActorReq(
        String actorId,
        String displayName) {
        return Messages.EnsurePlayerActorReq.newBuilder()
            .setActorId(actorId)
            .setDisplayName(displayName)
            .build();
    }

    public static Messages.EnsurePlayerActorRes ensurePlayerActorRes(
        String actorId,
        String actorType) {
        return Messages.EnsurePlayerActorRes.newBuilder()
            .setActorId(actorId)
            .setActorType(actorType)
            .build();
    }

    public static Messages.MatchBingoReq matchBingoReq(String mode) {
        return Messages.MatchBingoReq.newBuilder()
            .setMode(mode)
            .build();
    }

    public static Messages.MatchBingoRes matchBingoRes(
        String roomId,
        Messages.BingoRoomState state) {
        return Messages.MatchBingoRes.newBuilder()
            .setRoomId(roomId)
            .setState(state)
            .build();
    }

    public static Messages.MatchBingoApiReq matchBingoApiReq(
        String actorId,
        String displayName,
        String mode) {
        return Messages.MatchBingoApiReq.newBuilder()
            .setActorId(actorId)
            .setDisplayName(displayName)
            .setMode(mode)
            .build();
    }

    public static Messages.MatchBingoApiRes matchBingoApiRes(String roomId) {
        return Messages.MatchBingoApiRes.newBuilder()
            .setRoomId(roomId)
            .build();
    }

    public static Messages.ReserveBingoRoomReq reserveBingoRoomReq(
        String mode,
        String actorId,
        String levelBucket) {
        return Messages.ReserveBingoRoomReq.newBuilder()
            .setMode(mode)
            .setActorId(actorId)
            .setLevelBucket(levelBucket)
            .build();
    }

    public static Messages.ReserveBingoRoomRes reserveBingoRoomRes(
        String roomId,
        Messages.BingoRoomSettingsPayload settings) {
        return Messages.ReserveBingoRoomRes.newBuilder()
            .setRoomId(roomId)
            .setSettings(settings)
            .build();
    }

    public static Messages.BingoRoomJoinReq bingoRoomJoinReq(
        String roomId,
        String actorId,
        String displayName,
        boolean observeOnly) {
        return Messages.BingoRoomJoinReq.newBuilder()
            .setRoomId(roomId)
            .setActorId(actorId)
            .setDisplayName(displayName)
            .setObserveOnly(observeOnly)
            .build();
    }

    public static Messages.BingoRoomJoinRes bingoRoomJoinRes(Messages.BingoRoomState state) {
        return Messages.BingoRoomJoinRes.newBuilder()
            .setState(state)
            .build();
    }

    public static Messages.SubmitBingoCardReq submitBingoCardReq(String roomId, List<Integer> card) {
        return Messages.SubmitBingoCardReq.newBuilder()
            .setRoomId(roomId)
            .addAllCard(card)
            .build();
    }

    public static Messages.SubmitBingoCardRes submitBingoCardRes(Messages.BingoRoomState state) {
        return Messages.SubmitBingoCardRes.newBuilder()
            .setState(state)
            .build();
    }

    public static Messages.ObserveBingoEventsReq observeBingoEventsReq(String roomId) {
        return Messages.ObserveBingoEventsReq.newBuilder()
            .setRoomId(roomId)
            .build();
    }

    public static Messages.ObserveBingoEventsRes observeBingoEventsRes(boolean subscribed) {
        return Messages.ObserveBingoEventsRes.newBuilder()
            .setSubscribed(subscribed)
            .build();
    }

    public static Messages.StopObservingBingoEventsReq stopObservingBingoEventsReq(String roomId) {
        return Messages.StopObservingBingoEventsReq.newBuilder()
            .setRoomId(roomId)
            .build();
    }

    public static Messages.StopObservingBingoEventsRes stopObservingBingoEventsRes(
        boolean stopped) {
        return Messages.StopObservingBingoEventsRes.newBuilder()
            .setStopped(stopped)
            .build();
    }

    public static Messages.PlayerJoinedNotify playerJoinedNotify(
        String roomId,
        String actorId,
        String displayName,
        int seat,
        boolean host,
        Messages.BingoRoomState state) {
        return Messages.PlayerJoinedNotify.newBuilder()
            .setRoomId(roomId)
            .setActorId(actorId)
            .setDisplayName(displayName)
            .setSeat(seat)
            .setIsHost(host)
            .setState(state)
            .build();
    }

    public static Messages.BingoGameStartedNotify bingoGameStartedNotify(Messages.BingoRoomState state) {
        return Messages.BingoGameStartedNotify.newBuilder()
            .setState(state)
            .build();
    }

    public static Messages.BingoNumberDrawnNotify bingoNumberDrawnNotify(
        String roomId,
        int drawSeq,
        int number,
        Messages.BingoRoomState state) {
        return Messages.BingoNumberDrawnNotify.newBuilder()
            .setRoomId(roomId)
            .setDrawSeq(drawSeq)
            .setNumber(number)
            .setState(state)
            .build();
    }

    public static Messages.BingoStateNotify bingoStateNotify(Messages.BingoRoomState state) {
        return Messages.BingoStateNotify.newBuilder()
            .setState(state)
            .build();
    }

    public static Messages.BingoGameEndedNotify bingoGameEndedNotify(Messages.BingoRoomState state) {
        return Messages.BingoGameEndedNotify.newBuilder()
            .setState(state)
            .build();
    }

    public static Messages.BingoRewardAnnouncedNotify bingoRewardAnnouncedNotify(
        String roomId,
        String actorId,
        int drawSeq,
        String itemId,
        String itemName,
        String rarity) {
        return Messages.BingoRewardAnnouncedNotify.newBuilder()
            .setRoomId(roomId)
            .setActorId(actorId)
            .setDrawSeq(drawSeq)
            .setItemId(itemId)
            .setItemName(itemName)
            .setRarity(rarity)
            .build();
    }

    public static Messages.BingoRewardAcquiredEvent bingoRewardAcquiredEvent(
        String roomId,
        String actorId,
        int drawSeq,
        String itemId,
        String itemName,
        String rarity) {
        return Messages.BingoRewardAcquiredEvent.newBuilder()
            .setRoomId(roomId)
            .setActorId(actorId)
            .setDrawSeq(drawSeq)
            .setItemId(itemId)
            .setItemName(itemName)
            .setRarity(rarity)
            .build();
    }

    public static Messages.BingoRoomState bingoRoomState(
        String roomId,
        String status,
        String hostActorId,
        boolean canStart,
        int drawSeq,
        Integer lastDrawnNumber,
        List<Integer> drawnNumbers,
        List<Messages.BingoPlayerState> players,
        List<String> winners) {
        Messages.BingoRoomState.Builder builder = Messages.BingoRoomState.newBuilder()
            .setRoomId(roomId)
            .setStatus(status)
            .setHostActorId(hostActorId)
            .setCanStart(canStart)
            .setDrawSeq(drawSeq)
            .addAllDrawnNumbers(drawnNumbers)
            .addAllPlayers(players)
            .addAllWinners(winners);
        if (lastDrawnNumber != null) {
            builder.setLastDrawnNumber(lastDrawnNumber);
        }
        return builder.build();
    }

    public static Messages.BingoPlayerState bingoPlayerState(
        String actorId,
        String displayName,
        int seat,
        boolean host,
        List<Integer> card,
        List<Boolean> marks,
        int completedLines,
        int wins,
        int losses) {
        return Messages.BingoPlayerState.newBuilder()
            .setActorId(actorId)
            .setDisplayName(displayName)
            .setSeat(seat)
            .setIsHost(host)
            .addAllCard(card)
            .addAllMarks(marks)
            .setCompletedLines(completedLines)
            .setWins(wins)
            .setLosses(losses)
            .build();
    }
}
