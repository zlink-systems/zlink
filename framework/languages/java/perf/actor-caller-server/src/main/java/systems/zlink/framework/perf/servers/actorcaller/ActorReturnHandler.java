package systems.zlink.framework.perf.servers.actorcaller;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.SendSendCorrelation;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The return Channel handler of this caller (§10.10): the Actor's echo arrives as a second one-way send.
public final class ActorReturnHandler implements ZLinkSendHandler<PerfEchoReply> {
    private final SendSendCorrelation correlations;

    public ActorReturnHandler(SendSendCorrelation correlations) {
        this.correlations = correlations;
    }

    @Override
    public CompletionStage<Void> handle(PerfEchoReply message, ZLinkMessageContext context) {
        correlations.reply(message);
        return CompletableFuture.completedFuture(null);
    }
}
