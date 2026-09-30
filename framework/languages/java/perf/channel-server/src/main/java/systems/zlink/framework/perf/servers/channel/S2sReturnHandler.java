package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.SendSendCorrelation;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The return Channel handler of this caller (§10.4): the Spot's echo arrives as a second one-way send.
public final class S2sReturnHandler implements ZLinkSendHandler<PerfEchoReply> {
    private final SendSendCorrelation correlations;

    public S2sReturnHandler(SendSendCorrelation correlations) {
        this.correlations = correlations;
    }

    @Override
    public CompletionStage<Void> handle(PerfEchoReply message, ZLinkMessageContext context) {
        correlations.reply(message);
        return CompletableFuture.completedFuture(null);
    }
}
