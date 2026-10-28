package systems.zlink.contracts.messaging;

import systems.zlink.internal.ContractAccess;

public final class ReceivedTesting {
    private ReceivedTesting() {}

    public static ReplyToken replyToken(Object owner, long value) {
        return ContractAccess.replyToken(owner, value);
    }

    public static void setRouteGeneration(Received received, long generation) {
        ContractAccess.receivedSetRouteGeneration(received, generation);
    }

    public static boolean isClosed(Received received) {
        try {
            received.parts();
            return false;
        } catch (IllegalStateException closed) {
            return true;
        }
    }
}
