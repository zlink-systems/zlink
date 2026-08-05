/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.messaging.Message;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

public final class MessagePartsBuffer {
    private Message singlePart;
    private ArrayList<Message> parts;
    private List<Message> view;

    public void add(Message part) {
        Objects.requireNonNull(part, "part");
        view = null;
        if (parts != null) {
            parts.add(part);
            return;
        }
        if (singlePart == null) {
            singlePart = part;
            return;
        }
        parts = new ArrayList<>(4);
        parts.add(singlePart);
        parts.add(part);
        singlePart = null;
    }

    public boolean isEmpty() {
        return singlePart == null && (parts == null || parts.isEmpty());
    }

    public int size() {
        return parts != null ? parts.size() : singlePart == null ? 0 : 1;
    }

    public Message get(int index) {
        if (parts != null)
            return parts.get(index);
        if (index == 0 && singlePart != null)
            return singlePart;
        throw new IndexOutOfBoundsException(index);
    }

    public List<Message> asList() {
        if (parts != null)
            return parts;
        if (view == null)
            view = singlePart == null ? List.of() : List.of(singlePart);
        return view;
    }

    public MemorySegment copyToNativeArray(Arena arena) {
        Objects.requireNonNull(arena, "arena");
        int count = size();
        if (count == 0)
            return MemorySegment.NULL;
        MemorySegment out = arena.allocate(NativeLayouts.MESSAGE_LAYOUT, count);
        long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        int copied = 0;
        try {
            for (int i = 0; i < count; i++) {
                InternalAccess.messageCopyTo(get(i),
                  out.asSlice(i * stride, stride));
                copied++;
            }
            return out;
        } catch (RuntimeException | Error e) {
            closeNativeArray(out, copied);
            throw e;
        }
    }

    public static void closeNativeArray(MemorySegment parts, int count) {
        if (parts == MemorySegment.NULL || count <= 0)
            return;
        long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        for (int i = 0; i < count; i++) {
            NativeMessage.messageClose(parts.asSlice(i * stride, stride));
        }
    }
}
