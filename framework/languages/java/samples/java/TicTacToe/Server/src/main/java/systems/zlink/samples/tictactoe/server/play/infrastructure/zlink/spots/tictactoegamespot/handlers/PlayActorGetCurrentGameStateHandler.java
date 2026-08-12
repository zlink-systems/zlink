package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameMsg;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameNotify;

@ZLinkHandlerGroup(SampleNames.PlayActor)
public final class PlayActorGetCurrentGameStateHandler {
    @ZLinkSpotActorSend
    public CompletionStage<Void> currentState(
        TicTacToeGame spot,
        PlayActor actor,
        ZLinkMessageContext context,
        JoinGameMsg request) {
        return actor.context().boundSession()
            .send(new JoinGameNotify(spot.currentState(actor, request.roomId())))
            .submit();
    }
}
