@file:JvmName("ZLinkLocationExtensionsKt")
@file:JvmMultifileClass

package systems.zlink.framework.kotlin

import systems.zlink.framework.locations.ZLinkLocationPage
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery
import systems.zlink.framework.locations.ZLinkLocationRuntimeStatus
import systems.zlink.framework.locations.ZLinkLocationServiceSummary
import systems.zlink.framework.locations.ZLinkLocationServiceSummaryFilter
import systems.zlink.framework.locations.ZLinkLocationTopologyEntry
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter
import systems.zlink.framework.locations.ZLinkPageRequest

suspend fun ZLinkLocationRuntimeQuery.status(): ZLinkLocationRuntimeStatus =
    awaitFrameworkStage(status)

suspend fun ZLinkLocationRuntimeQuery.listTopology(
    filter: ZLinkLocationTopologyFilter,
    page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationTopologyEntry> =
    awaitFrameworkStage(listTopology(filter, page))

suspend fun ZLinkLocationRuntimeQuery.listServiceSummaries(
    filter: ZLinkLocationServiceSummaryFilter,
    page: ZLinkPageRequest = ZLinkPageRequest.firstPage(),
): ZLinkLocationPage<ZLinkLocationServiceSummary> =
    awaitFrameworkStage(listServiceSummaries(filter, page))
