package systems.zlink.framework.runtime.internal.service;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.function.Consumer;

/**
 * Bounded level-ready mailbox scheduler shared by service runtime domains.
 *
 * <p>Admission, budget accounting, and the empty-to-ready transition occur
 * under one monitor. A mailbox can have only one active claim, matching the
 * preserved Core owner-mailbox state machine without exposing claim handles
 * through the Framework public API.
 */
final class ZLinkServiceMailboxScheduler {
    enum Domain {
        APPLICATION,
        INFRASTRUCTURE
    }

    record Owner(String kind, String key, long generation) {
        Owner {
            Objects.requireNonNull(kind, "kind");
            Objects.requireNonNull(key, "key");
            if (generation < 0) {
                throw new IllegalArgumentException("generation must not be negative");
            }
        }
    }

    record Work(long retainedBytes, Runnable action) {
        Work {
            if (retainedBytes < 0) {
                throw new IllegalArgumentException("retainedBytes must not be negative");
            }
            Objects.requireNonNull(action, "action");
        }
    }

    enum Admission {
        ACCEPTED,
        SEALED,
        BACKPRESSURED
    }

    private final long messageLimit;
    private final long byteLimit;
    private final Map<Key, Mailbox> mailboxes = new HashMap<>();
    private final Set<Key> ready = new LinkedHashSet<>();
    private boolean sealed;

    ZLinkServiceMailboxScheduler(long messageLimit, long byteLimit) {
        if (messageLimit <= 0 || byteLimit <= 0) {
            throw new IllegalArgumentException("mailbox limits must be positive");
        }
        this.messageLimit = messageLimit;
        this.byteLimit = byteLimit;
    }

    synchronized Admission admit(Owner owner, Domain domain, Work work) {
        Objects.requireNonNull(owner, "owner");
        Objects.requireNonNull(domain, "domain");
        Objects.requireNonNull(work, "work");
        if (sealed) {
            return Admission.SEALED;
        }
        Key key = new Key(owner, domain);
        Mailbox mailbox = mailboxes.computeIfAbsent(key, ignored -> new Mailbox());
        if (mailbox.queue.size() >= messageLimit
            || work.retainedBytes() > byteLimit - mailbox.pendingBytes) {
            return Admission.BACKPRESSURED;
        }
        boolean wasEmpty = mailbox.queue.isEmpty();
        mailbox.queue.addLast(work);
        mailbox.pendingBytes += work.retainedBytes();
        if (wasEmpty && !mailbox.claimed) {
            ready.add(key);
        }
        return Admission.ACCEPTED;
    }

    synchronized Owner pollReady(Domain domain) {
        Objects.requireNonNull(domain, "domain");
        for (Key key : ready) {
            if (key.domain == domain) {
                return key.owner;
            }
        }
        return null;
    }

    int drain(Owner owner, Domain domain, int maximumWork, Consumer<Work> consumer) {
        Objects.requireNonNull(consumer, "consumer");
        if (maximumWork <= 0) {
            throw new IllegalArgumentException("maximumWork must be positive");
        }
        Key key = new Key(owner, domain);
        synchronized (this) {
            Mailbox mailbox = mailboxes.get(key);
            if (mailbox == null || mailbox.claimed || mailbox.queue.isEmpty()) {
                return 0;
            }
            mailbox.claimed = true;
            ready.remove(key);
        }

        int completed = 0;
        try {
            while (completed < maximumWork) {
                Work work;
                synchronized (this) {
                    Mailbox mailbox = mailboxes.get(key);
                    work = mailbox.queue.pollFirst();
                    if (work == null) {
                        break;
                    }
                    mailbox.pendingBytes -= work.retainedBytes();
                }
                consumer.accept(work);
                completed++;
            }
            return completed;
        } finally {
            synchronized (this) {
                Mailbox mailbox = mailboxes.get(key);
                mailbox.claimed = false;
                if (!mailbox.queue.isEmpty()) {
                    ready.add(key);
                } else {
                    mailboxes.remove(key);
                }
            }
        }
    }

    synchronized void seal() {
        sealed = true;
    }

    synchronized long pendingMessages() {
        return mailboxes.values().stream().mapToLong(mailbox -> mailbox.queue.size()).sum();
    }

    synchronized long pendingBytes() {
        return mailboxes.values().stream().mapToLong(mailbox -> mailbox.pendingBytes).sum();
    }

    private record Key(Owner owner, Domain domain) {
    }

    private static final class Mailbox {
        private final ArrayDeque<Work> queue = new ArrayDeque<>();
        private long pendingBytes;
        private boolean claimed;
    }
}
