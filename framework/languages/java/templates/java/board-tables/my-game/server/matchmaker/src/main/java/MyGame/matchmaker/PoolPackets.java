package MyGame.matchmaker;

import MyGame.*;

import systems.zlink.framework.spots.*;

import java.util.concurrent.CompletionStage;

public final class PoolPackets {
    private PoolPackets() {}

    public static final class Enter implements ZLinkSpotPacketHandler<Pool, EnterReport> {
        public CompletionStage<Void> handle(Pool pool, EnterReport message) {
            return pool.report(message);
        }
    }

    public static final class Tick implements ZLinkSpotTimerHandler<Pool> {
        public CompletionStage<Void> handle(Pool pool, ZLinkTimerTick tick) {
            return pool.tick();
        }
    }
}
