package systems.zlink.framework.runtime.internal.binding.spot;

import java.util.ArrayList;
import java.util.List;
import systems.zlink.contracts.messaging.Message;

/** Framework-owned reusable receive storage. */
final class FrameworkReceiveBatch implements ReceiveBatch {
    private final int messageCapacity;
    private final int partCapacity;
    private final int byteCapacity;
    private final List<Entry> entries = new ArrayList<>();
    private boolean closed;

    FrameworkReceiveBatch(
        int messageCapacity,
        int partCapacity,
        int byteCapacity) {
        if (messageCapacity <= 0 || partCapacity <= 0 || byteCapacity <= 0) {
            throw new IllegalArgumentException(
                "receive batch capacities must be positive");
        }
        this.messageCapacity = messageCapacity;
        this.partCapacity = partCapacity;
        this.byteCapacity = byteCapacity;
    }

    @Override
    public int count() {
        ensureOpen();
        return entries.size();
    }

    @Override
    public ReceiveRecord at(int index) {
        ensureOpen();
        return entries.get(index).record();
    }

    @Override
    public List<Message> retainMessage(int index) {
        ensureOpen();
        return entries.get(index).parts().stream()
            .map(bytes -> Message.from(bytes.clone()))
            .toList();
    }

    @Override
    public void reset() {
        ensureOpen();
        entries.clear();
    }

    @Override
    public void close() {
        closed = true;
        entries.clear();
    }

    boolean tryAdd(ReceiveRecord record, List<byte[]> parts) {
        ensureOpen();
        int currentParts = entries.stream()
            .mapToInt(entry -> entry.parts().size())
            .sum();
        int currentBytes = entries.stream()
            .flatMap(entry -> entry.parts().stream())
            .mapToInt(bytes -> bytes.length)
            .sum();
        int addedBytes = parts.stream().mapToInt(bytes -> bytes.length).sum();
        if (entries.size() == messageCapacity
            || currentParts + parts.size() > partCapacity
            || currentBytes + addedBytes > byteCapacity) {
            return false;
        }
        entries.add(new Entry(
            record,
            parts.stream().map(byte[]::clone).toList()));
        return true;
    }

    private void ensureOpen() {
        if (closed) {
            throw new IllegalStateException("receive batch is closed");
        }
    }

    private record Entry(ReceiveRecord record, List<byte[]> parts) {
    }
}
