package MyGame.table;

import MyGame.*;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.concurrent.CompletionStage;

public final class RoomPackets {
    private RoomPackets() {}

    public static final class Prepare implements ZLinkSpotPacketHandler<Table, PrepareRoom> {
        public CompletionStage<Void> handle(Table table, PrepareRoom message) {
            return table.context()
                    .outbound()
                    .sendToSpot(Names.POOL_KEY, table.prepare(message))
                    .instanceSpot(Names.POOL)
                    .inMesh(Names.MESH)
                    .submit();
        }
    }

    public static final class Commit implements ZLinkSpotPacketHandler<Table, CommitRoom> {
        public CompletionStage<Void> handle(Table table, CommitRoom message) {
            return table.commit(message);
        }
    }

    public static final class Withdraw implements ZLinkSpotPacketHandler<Table, WithdrawRoom> {
        public CompletionStage<Void> handle(Table table, WithdrawRoom message) {
            return table.withdraw(message);
        }
    }
}
