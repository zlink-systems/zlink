package MyGame.table;

import MyGame.*;

import systems.zlink.framework.handlers.*;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.TABLE)
public final class RoomPackets {
    @ZLinkSpotRequest
    public CompletionStage<RoomPrepared> prepare(Table table, PrepareRoom request) {
        return CompletableFuture.completedFuture(table.prepare(request));
    }

    public static final class Commit implements ZLinkSpotPacketHandler<Table, CommitRoom> {
        public CompletionStage<Void> handle(Table table, CommitRoom message) {
            return table.commit(message);
        }
    }

    public static final class Withdraw implements ZLinkSpotPacketHandler<Table, WithdrawRoom> {
        public CompletionStage<Void> handle(Table table, WithdrawRoom message) {
            table.withdraw(message);
            return CompletableFuture.completedFuture(null);
        }
    }
}
