package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.server.play.domain.bingo.BingoRoomModels;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class BingoRoomRelocationAdapter
    implements ZLinkSpotRelocationAdapter<BingoRoomSpot> {
    @Override
    public CompletionStage<byte[]> capture(
        BingoRoomSpot spot,
        ZLinkRelocationCancellation cancellation) {
        ensureActive(cancellation);
        try {
            BingoRoomSpot.RelocationState state = spot.captureRelocationState();
            Messages.BingoRoomSettingsPayload settings =
                Messages.BingoRoomSettingsPayload.newBuilder()
                    .setRoomName(state.settings().roomName())
                    .setMode(state.settings().mode())
                    .setRequiredPlayers(state.settings().requiredPlayers())
                    .setMaxDrawNumber(state.settings().maxDrawNumber())
                    .setPurpose(state.settings().purpose())
                    .setObservedRoomId(state.settings().observedRoomId() == null
                        ? ""
                        : state.settings().observedRoomId())
                    .build();
            byte[] settingsBytes = settings.toByteArray();
            byte[] stateBytes = state.state().toByteArray();
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (DataOutputStream output = new DataOutputStream(bytes)) {
                output.writeInt(settingsBytes.length);
                output.write(settingsBytes);
                output.write(stateBytes);
            }
            return CompletableFuture.completedFuture(bytes.toByteArray());
        } catch (Exception failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    @Override
    public CompletionStage<Void> restore(
        BingoRoomSpot spot,
        byte[] payload,
        ZLinkRelocationCancellation cancellation) {
        ensureActive(cancellation);
        try (DataInputStream input = new DataInputStream(
            new ByteArrayInputStream(payload))) {
            int settingsLength = input.readInt();
            if (settingsLength < 0 || settingsLength > payload.length - Integer.BYTES) {
                throw new IllegalArgumentException("invalid Bingo relocation settings length");
            }
            Messages.BingoRoomSettingsPayload settings =
                Messages.BingoRoomSettingsPayload.parseFrom(
                    input.readNBytes(settingsLength));
            Messages.BingoRoomState state =
                Messages.BingoRoomState.parseFrom(input.readAllBytes());
            spot.restoreRelocationState(new BingoRoomSpot.RelocationState(
                new BingoRoomModels.BingoRoomSettings(
                    settings.getRoomName(),
                    settings.getMode(),
                    settings.getRequiredPlayers(),
                    settings.getMaxDrawNumber(),
                    SampleTimings.DrawPeriod.toMillis(),
                    settings.getPurpose(),
                    settings.getObservedRoomId().isBlank()
                        ? null
                        : settings.getObservedRoomId()),
                state));
            return CompletableFuture.completedFuture(null);
        } catch (Exception failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private static void ensureActive(ZLinkRelocationCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            throw new java.util.concurrent.CancellationException(
                "Bingo room relocation was cancelled");
        }
    }
}
