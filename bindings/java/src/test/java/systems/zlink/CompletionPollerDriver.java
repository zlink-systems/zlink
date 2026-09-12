/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.sockets.Socket;

/** Drives the public completion owner for async binding contract tests. */
public final class CompletionPollerDriver implements AutoCloseable {
    private static final Duration WAIT_SLICE = Duration.ofMillis(25);
    private final Poller poller = Zlink.createPoller();
    private final AtomicBoolean stopping = new AtomicBoolean();
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final Thread thread;

    public CompletionPollerDriver(Socket... sockets) {
        Objects.requireNonNull(sockets, "sockets");
        if (sockets.length == 0)
            throw new IllegalArgumentException("at least one socket required");
        for (int index = 0; index < sockets.length; index++) {
            poller.add(Objects.requireNonNull(sockets[index], "sockets[" + index
                + "]"), index, PollEventFlags.POLLCOMPLETION);
        }
        thread = Thread.ofPlatform().daemon(true)
            .name("zlink-test-public-completion-owner")
            .start(() -> run(sockets.length));
    }

    private void run(int capacity) {
        PollEvents events = new PollEvents(capacity);
        try {
            while (!stopping.get())
                poller.wait(events, WAIT_SLICE);
        } catch (Throwable stopped) {
            if (!stopping.get())
                failure.compareAndSet(null, stopped);
        }
    }

    @Override
    public void close() {
        stopping.set(true);
        boolean interrupted = false;
        while (thread.isAlive()) {
            try {
                thread.join(TestSupport.DEFAULT_TIMEOUT_MS);
            } catch (InterruptedException ignored) {
                interrupted = true;
            }
        }
        poller.close();
        if (interrupted)
            Thread.currentThread().interrupt();
        Throwable stopped = failure.get();
        if (stopped != null)
            throw new AssertionError("public completion poller failed", stopped);
    }
}
