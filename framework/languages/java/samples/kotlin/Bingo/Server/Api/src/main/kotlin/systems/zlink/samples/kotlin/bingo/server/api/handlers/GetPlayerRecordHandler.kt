package systems.zlink.samples.kotlin.bingo.server.api.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.samples.kotlin.bingo.server.api.BingoPlayerRecordStore
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.shared.contracts.GetPlayerRecordReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.GetPlayerRecordRes

@ZLinkHandlerGroup(SampleNames.ApiChannel)
class GetPlayerRecordHandler(private val records: BingoPlayerRecordStore) :
    ZLinkSuspendingRequestHandler<GetPlayerRecordReq, GetPlayerRecordRes> {
    override suspend fun handle(request: GetPlayerRecordReq, context: ZLinkMessageContext): GetPlayerRecordRes {
        val record = records.get(request.actorId)
        return GetPlayerRecordRes(record.actorId, record.wins, record.losses)
    }
}
