package systems.zlink.framework.docexamples.stream;

import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.streams.ZLinkSessionContext;

/** 가이드 9장 §1·§6 — 등록과 server push. */
public final class StreamNodeRegistration {
    private StreamNodeRegistration() {
    }

    public static void register(ZLinkFrameworkOptions options) {
        // --8<-- [start:register-stream-node]
        options.addStreamNode("client-stream")
            .bind("tcp://0.0.0.0:9100")
            .enableActorDispatch()                  // Actor authority mesh는 Framework가 선택한다.
            .registerSession(PlayStreamSession.class); // 연결마다 만들 session type을 등록한다.
        // --8<-- [end:register-stream-node]
    }

    public static void notifyClient(ZLinkSessionContext context) {
        // --8<-- [start:server-push]
        // local transport queue admission까지 기다린다.
        context.client()
            .send(new StreamMessages.ServerNotice("maintenance"))
            .metadata("severity", "info")
            .compress()
            .submit();
        // --8<-- [end:server-push]
    }
}
