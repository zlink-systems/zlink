package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;
import systems.zlink.samples.tictactoe.shared.contracts.TicTacToeGameCreateReq;

public final class TicTacToeGameCreatedHandler {
    public ZLinkSpotCreateResponse handle(
        TicTacToeGame game,
        ZLinkMessage request) {
        game.markCreated(request.decode(TicTacToeGameCreateReq.class));
        return ZLinkSpotCreateResponse.accept();
    }
}
