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
public final class ReportBingoResultHandler
    implements ZLinkRequestHandler<Messages.ReportBingoResultReq, Messages.ReportBingoResultRes> {
    private final BingoPlayerRecordStore records;

    public ReportBingoResultHandler(BingoPlayerRecordStore records) {
        this.records = records;
    }

    @Override
    public CompletionStage<Messages.ReportBingoResultRes> handle(
        Messages.ReportBingoResultReq request, ZLinkMessageContext context) {
        var record = records.report(request.getActorId(), request.getWon());
        return CompletableFuture.completedFuture(BingoMessages.reportBingoResultRes(
            record.actorId(), record.wins(), record.losses()));
    }
}
