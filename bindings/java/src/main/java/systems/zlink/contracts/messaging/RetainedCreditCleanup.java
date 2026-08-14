/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.lang.ref.Cleaner;

/** Owns one opaque retained-credit release action for reusable result state. */
final class RetainedCreditCleanup implements Runnable {
    private static final Cleaner CLEANER = Cleaner.create();

    private Runnable release;

    Cleaner.Cleanable register(Object owner) {
        return CLEANER.register(owner, this);
    }

    void replace(Runnable next) {
        Runnable previous;
        synchronized (this) {
            previous = release;
            release = next;
        }
        runQuietly(previous);
    }

    void transferFrom(RetainedCreditCleanup source) {
        replace(source.detach());
    }

    void release() {
        runQuietly(detach());
    }

    @Override
    public void run() {
        release();
    }

    private synchronized Runnable detach() {
        Runnable detached = release;
        release = null;
        return detached;
    }

    private static void runQuietly(Runnable action) {
        if (action == null)
            return;
        try {
            action.run();
        } catch (RuntimeException ignored) {
        }
    }
}
