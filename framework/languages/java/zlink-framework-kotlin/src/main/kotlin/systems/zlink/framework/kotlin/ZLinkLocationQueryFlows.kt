@file:JvmName("ZLinkLocationExtensionsKt")
@file:JvmMultifileClass

package systems.zlink.framework.kotlin

import java.util.concurrent.CompletionStage
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import systems.zlink.framework.locations.ZLinkLocationPage
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery
import systems.zlink.framework.locations.ZLinkLocationTopologyEntry
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter
import systems.zlink.framework.locations.ZLinkPageRequest

@JvmSynthetic
internal fun <T> locationPages(
    firstPage: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
    load: (ZLinkPageRequest) -> CompletionStage<ZLinkLocationPage<T>>,
): Flow<T> = flow {
    var request = firstPage
    while (true) {
        val page = awaitFrameworkStage(load(request))
        for (item in page.items()) {
            emit(item)
        }
        val next = page.continuationToken()
        if (next.isNullOrBlank()) {
            break
        }
        request = ZLinkPageRequest(request.pageSize(), next)
    }
}

fun ZLinkLocationRuntimeQuery.topology(
    filter: ZLinkLocationTopologyFilter,
    pageSize: Int = 100,
): Flow<ZLinkLocationTopologyEntry> =
    locationPages(ZLinkPageRequest(pageSize, null)) { page -> listTopology(filter, page) }
