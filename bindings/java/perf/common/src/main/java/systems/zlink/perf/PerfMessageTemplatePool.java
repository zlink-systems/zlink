/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;

/**
 * Reuses immutable native payload storage after all shared send references
 * have been released by Core. The returned Message is an independent shared
 * owner and keeps the public send ownership contract unchanged.
 */
public final class PerfMessageTemplatePool implements AutoCloseable {
    private final Message[] templates;
    private int cursor;

    public PerfMessageTemplatePool(int size, int capacity) {
        if (capacity <= 0) {
            throw new IllegalArgumentException("capacity must be > 0");
        }
        templates = new Message[capacity];
        for (int i = 0; i < capacity; i++) {
            templates[i] = PerfUtil.payloadTemplate(size);
        }
    }

    public Message acquire(int size, byte phase, long sentNanoTime,
                           long deadlineNanoTime) {
        while (System.nanoTime() < deadlineNanoTime) {
            for (int offset = 0; offset < templates.length; offset++) {
                int index = cursor + offset;
                if (index >= templates.length) {
                    index -= templates.length;
                }
                Message template = templates[index];
                if (template.refCount() != 1) {
                    continue;
                }
                PerfUtil.writePayloadHeader(template, size, phase,
                    sentNanoTime);
                cursor = index + 1;
                if (cursor == templates.length) {
                    cursor = 0;
                }
                return Message.from(template);
            }
            Thread.onSpinWait();
        }
        return null;
    }

    @Override
    public void close() {
        Message.closeAll(templates);
    }
}
