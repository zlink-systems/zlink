package systems.zlink.samples.bingo.server.api.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.bingo.server.api.BingoPlayerRecordStore;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.ApiChannel)
public final class GetPlayerRecordHandler
    implements ZLinkRequestHandler<Messages.GetPlayerRecordReq, Messages.GetPlayerRecordRes> {
    private final BingoPlayerRecordStore records;

    public GetPlayerRecordHandler(BingoPlayerRecordStore records) {
        this.records = records;
    }

    @Override
    public CompletionStage<Messages.GetPlayerRecordRes> handle(
        Messages.GetPlayerRecordReq request, ZLinkMessageContext context) {
        var record = records.get(request.getActorId());
        return CompletableFuture.completedFuture(BingoMessages.getPlayerRecordRes(
            record.actorId(), record.wins(), record.losses()));
    }
}
