package systems.zlink.tutorial.server.channel;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.tutorial.shared.Contracts;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// --8<-- [start:fanout-handler]
// Receives what any publisher on this channel sends. The publisher does not know
// this node exists, so adding or removing a subscriber changes nothing there.
public final class MaintenanceNoticeSubscriber
        implements ZLinkFanoutHandler<Contracts.MaintenanceNotice> {

    private static final Logger LOG = LoggerFactory.getLogger(MaintenanceNoticeSubscriber.class);

    @Override
    public CompletionStage<Void> handle(
            Contracts.MaintenanceNotice message, ZLinkPublishMessageContext context) {
        LOG.info("maintenance notice: {}", message.message());
        return CompletableFuture.completedFuture(null);
    }
}
// --8<-- [end:fanout-handler]
