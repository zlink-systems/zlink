package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.handlers;
import java.util.concurrent.CompletionStage;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors.PlayActor;
import systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.spots.tictactoegamespot.TicTacToeGame;
import systems.zlink.samples.tictactoe.shared.contracts.LeaveGameMsg;

@ZLinkHandlerGroup(SampleNames.PlayActor)
public final class PlayActorLeaveGameHandler {
    private static final Logger LOGGER = LoggerFactory.getLogger(
        PlayActorLeaveGameHandler.class);

    @ZLinkSpotActorSend
    public CompletionStage<Void> leaveGame(
        TicTacToeGame spot,
        PlayActor actor,
        ZLinkMessageContext context,
        LeaveGameMsg message) {
        LOGGER.info(
            "actor: LeaveGameMsg received. actor={}, roomId={}",
            actor.actorId(),
            message.roomId());
        return spot.leaveGame(actor, message.roomId())
            .thenRun(() -> LOGGER.info(
                "tictactoe-lifecycle leave-completed actor={}",
                actor.actorId()));
    }
}
