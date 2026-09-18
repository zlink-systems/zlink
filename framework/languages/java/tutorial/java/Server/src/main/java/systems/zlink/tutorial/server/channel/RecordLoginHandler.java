package systems.zlink.tutorial.server.channel;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.tutorial.shared.Contracts;

// --8<-- [start:channel-send-handler]
// Handles a one-way message. There is no return value, so the caller is already
// done by the time this runs and cannot observe a failure here.
public final class RecordLoginHandler implements ZLinkSendHandler<Contracts.RecordLogin> {

    // The Framework owns the handler instance and builds it through Spring, so a
    // constructor parameter would have to be a bean. A logger is not one; the
    // ordinary Java idiom is a static logger per class.
    private static final Logger LOG = LoggerFactory.getLogger(RecordLoginHandler.class);

    @Override
    public CompletionStage<Void> handle(
        Contracts.RecordLogin message,
        ZLinkMessageContext context) {
        LOG.info("login recorded: {}", message.playerId());
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:channel-send-handler]
