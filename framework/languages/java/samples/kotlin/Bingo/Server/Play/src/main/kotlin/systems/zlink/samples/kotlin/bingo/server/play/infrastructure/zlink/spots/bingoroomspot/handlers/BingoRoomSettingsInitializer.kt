package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers

import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.BingoRoomSpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomSettingsPayload

class BingoRoomSettingsInitializer {
    fun handle(
        spot: BingoRoomSpot,
        request: ZLinkMessage,
    ) {
        spot.applySettings(decodeSettings(request))
    }

    private fun decodeSettings(request: ZLinkMessage): BingoRoomSettings {
        if (request.isEmpty()) {
            return BingoRoomSettings.create(
                "two-player",
                0,
                SampleTimings.DrawPeriod.toMillis(),
            )
        }
        val settings = request.decode(BingoRoomSettingsPayload::class.java)
        return BingoRoomSettings(
            roomName = settings.roomName,
            mode = settings.mode,
            requiredPlayers = settings.requiredPlayers,
            maxDrawNumber = settings.maxDrawNumber,
            drawPeriodMillis = SampleTimings.DrawPeriod.toMillis(),
            purpose = settings.purpose,
            observedRoomId = if (settings.hasObservedRoomId()) settings.observedRoomId else null,
        )
    }
}
