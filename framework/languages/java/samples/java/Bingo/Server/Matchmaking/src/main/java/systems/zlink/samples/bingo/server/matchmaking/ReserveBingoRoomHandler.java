package systems.zlink.samples.bingo.server.matchmaking;

import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.samples.bingo.shared.contracts.Messages;

import java.util.concurrent.CompletionStage;

public final class ReserveBingoRoomHandler
        implements ZLinkSpotRequestHandler<
                BingoMatchmaker, Messages.ReserveBingoRoomReq, Messages.ReserveBingoRoomRes> {
    private final RedisBingoMatchReservationStore reservations;

    public ReserveBingoRoomHandler(RedisBingoMatchReservationStore reservations) {
        this.reservations = reservations;
    }

    @Override
    public CompletionStage<Messages.ReserveBingoRoomRes> handle(
            BingoMatchmaker spot, Messages.ReserveBingoRoomReq request) {
        spot.beginRequest();
        return reservations.reserve(request).whenComplete((ignored, failure) -> spot.endRequest());
    }
}
