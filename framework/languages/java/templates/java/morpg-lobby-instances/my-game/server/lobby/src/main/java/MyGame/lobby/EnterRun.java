package MyGame.lobby;
import MyGame.*;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import java.util.concurrent.*;
public final class EnterRun implements ZLinkSpotPacketHandler<Party, EnterPartyRun> {
    public CompletionStage<Void> handle(Party spot, EnterPartyRun r) { return spot.enterPlayer(r.playerId()); }
}
