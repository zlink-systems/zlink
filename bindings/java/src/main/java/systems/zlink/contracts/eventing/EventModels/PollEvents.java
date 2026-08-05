/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import systems.zlink.internal.ContractAccess;
import java.util.Objects;

/** A pre-allocated result buffer for a poller wait; holds up to {@code capacity} ready events. */
public final class PollEvents {
    private final int capacity;
    private final PollSourceKind[] sourceKinds;
    private final long[] slots;
    private final int[] revents;
    private final int[] fds;
    private int readyCount;

    static {
        ContractAccess.register(new ContractAccess.PollEventsAccess() {
            @Override
            public void markReadyCount(PollEvents events, int readyCount) {
                events.markReadyCount(readyCount);
            }

            @Override
            public void markEvent(PollEvents events, int index,
                                  int sourceKindValue, long slot,
                                  int revents, int fd) {
                events.markEvent(index, sourceKindValue, slot, revents, fd);
            }
        });
    }

    public PollEvents(int capacity) {
        if (capacity <= 0)
            throw new IllegalArgumentException("capacity must be > 0");
        this.capacity = capacity;
        this.sourceKinds = new PollSourceKind[capacity];
        this.slots = new long[capacity];
        this.revents = new int[capacity];
        this.fds = new int[capacity];
    }

    public int capacity() {
        return capacity;
    }

    public int readyCount() {
        return readyCount;
    }

    public PollSourceKind sourceKind(int index) {
        checkReadyIndex(index);
        return sourceKinds[index];
    }

    public long slot(int index) {
        checkReadyIndex(index);
        return slots[index];
    }

    public int revents(int index) {
        checkReadyIndex(index);
        return revents[index];
    }

    public boolean hasEvent(int index, PollEventFlags event) {
        Objects.requireNonNull(event, "event");
        return (revents(index) & event.mask()) != 0;
    }

    public PollEvent eventAt(int index) {
        checkReadyIndex(index);
        return new PollEvent(sourceKinds[index], slots[index], revents[index],
            fds[index]);
    }

    public int fd(int index) {
        checkReadyIndex(index);
        return fds[index];
    }

    void markReadyCount(int readyCount) {
        if (readyCount < 0 || readyCount > capacity)
            throw new IllegalArgumentException("readyCount out of range");
        this.readyCount = readyCount;
    }

    void markEvent(int index, int sourceKindValue, long slot, int revents,
                   int fd) {
        if (index < 0 || index >= capacity)
            throw new IndexOutOfBoundsException("event index " + index);
        sourceKinds[index] = PollSourceKind.fromValue(sourceKindValue);
        slots[index] = slot;
        this.revents[index] = revents;
        fds[index] = fd;
    }

    private void checkReadyIndex(int index) {
        if (index < 0 || index >= readyCount)
            throw new IndexOutOfBoundsException("ready index " + index);
    }

}
