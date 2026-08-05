package systems.zlink.framework.docexamples.stream;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;

/** 가이드 9장 §2 — session lifecycle. */
// --8<-- [start:session-lifecycle]
public final class PlayStreamSession implements ZLinkSession {
    private final ZLinkSessionContext context;

    public PlayStreamSession(ZLinkSessionContext context) {
        this.context = context;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        // typed handler가 처리하지 않은 packet만 여기로 온다.
        // application protocol에 없는 packet을 받으면 연결을 닫는다.
        return context.close();
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        // session 범위의 transport 오류만 온다. handler 예외는 오지 않는다.
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:session-lifecycle]
