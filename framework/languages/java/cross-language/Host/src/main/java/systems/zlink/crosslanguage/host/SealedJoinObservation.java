package systems.zlink.crosslanguage.host;

import ch.qos.logback.classic.Logger;
import ch.qos.logback.classic.spi.ILoggingEvent;
import ch.qos.logback.core.AppenderBase;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;

/** Captures the Framework's own flow events for the sealed Join scenario. */
public final class SealedJoinObservation {
    private final EventSink sink;
    private final String actorId;
    private final String senderRid;
    private final boolean enabled;

    /** The sealed Join scenario injects a Join completion failure for its Actor. */
    static boolean injectsCompletionFailure() {
        return "1".equals(System.getenv("ZLINK_CROSS_FAIL_JOIN_COMPLETION"));
    }

    public SealedJoinObservation(HostArgs args, EventSink sink) {
        this.sink = sink;
        this.actorId = args.option("actor-id", "cross-lang-user-spot-actor");
        this.enabled = injectsCompletionFailure();
        this.senderRid = System.getenv("ZLINK_CROSS_LATE_SENDER_RID");
        if (!enabled) return;
        if (senderRid == null || senderRid.isBlank()) {
            throw new IllegalStateException("Missing ZLINK_CROSS_LATE_SENDER_RID.");
        }
        Path flowFile = Path.of(args.require("event-file") + ".flow");
        var appender = new AppenderBase<ILoggingEvent>() {
            @Override protected void append(ILoggingEvent event) {
                String line = event.getFormattedMessage();
                if (!line.startsWith("zlink flow:")) return;
                line = " " + line.replace("event_id=", "event=") + " ";
                try {
                    Files.writeString(flowFile, line + System.lineSeparator(), StandardCharsets.UTF_8,
                        StandardOpenOption.CREATE, StandardOpenOption.APPEND);
                } catch (IOException error) {
                    throw new UncheckedIOException(error);
                }
                observeIngress(line);
            }
        };
        appender.start();
        ((Logger) LoggerFactory.getLogger(ZLinkMessageFlowTracer.class)).addAppender(appender);
    }

    void failCompletion(String actor, String nodeRid) {
        if (!enabled || !actorId.equals(actor)) return;
        sink.append("late-actor-completion-throw|actor=" + actor + "|nodeRid=" + nodeRid);
        sink.append("late-actor-owner-resumed|nodeRid=" + nodeRid);
        throw new IllegalStateException("cross-language Join completion failure");
    }

    private void observeIngress(String line) {
        if (!line.contains(" event=zlink.dispatch_error ")
            || !line.contains(" source_rid=" + senderRid + " ")
            || !line.contains(" actor=" + actorId + " ")) return;
        String kind = line.contains(" kind=send ") ? "actorSend"
            : line.contains(" kind=request ") ? "actorRequest" : null;
        if (kind != null) {
            sink.append("late-actor-owner-ingress|kind=" + kind + "|source=" + senderRid
                + "|actor=" + actorId);
        }
    }
}
