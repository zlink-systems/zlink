package MyGame.table

import java.util.concurrent.CancellationException
import systems.zlink.framework.actors.ZLinkRelocationCancellation

fun ZLinkRelocationCancellation.ensureActive() {
    if (isCancellationRequested) throw CancellationException("Application state transfer cancelled")
}
