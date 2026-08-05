package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot;
import systems.zlink.samples.bingo.server.play.domain.bingo.BingoRoomModels;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class BingoRoomSettingsInitializer {
    public ZLinkSpotCreateResponse handle(
        BingoRoomSpot spot,
        ZLinkMessage request) {
        spot.applySettings(decodeSettings(request));
        return ZLinkSpotCreateResponse.accept();
    }

    private BingoRoomModels.BingoRoomSettings decodeSettings(ZLinkMessage request) {
        if (request.isEmpty()) {
            return BingoRoomModels.BingoRoomSettings.create(
                "two-player",
                0,
                SampleTimings.DrawPeriod.toMillis());
        }
        Messages.BingoRoomSettingsPayload settings =
            request.decode(Messages.BingoRoomSettingsPayload.class);
        return new BingoRoomModels.BingoRoomSettings(
            settings.getRoomName(),
            settings.getMode(),
            settings.getRequiredPlayers(),
            settings.getMaxDrawNumber(),
            SampleTimings.DrawPeriod.toMillis(),
            settings.getPurpose(),
            settings.hasObservedRoomId() ? settings.getObservedRoomId() : null);
    }
}
