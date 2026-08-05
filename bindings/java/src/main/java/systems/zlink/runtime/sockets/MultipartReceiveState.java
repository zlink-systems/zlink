/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.runtime.nativeapi.InternalAccess;
final class MultipartReceiveState {
    private static final Message[] NO_FRAMES = new Message[0];

    private Message[] frames = NO_FRAMES;
    private int index;

    boolean hasPending() {
        return index < frames.length;
    }

    int pendingCount() {
        return frames.length - index;
    }

    Message poll() {
        Message frame = frames[index++];
        InternalAccess.messageSetMore(frame, index < frames.length);
        if (!hasPending()) {
            frames = NO_FRAMES;
            index = 0;
        }
        return frame;
    }

    void replace(Message[] nextFrames) {
        closeRemaining();
        frames = nextFrames;
        index = 0;
    }

    void closeRemaining() {
        // HOT PATH: prepareRecvLikeOperation calls this before each recv-like
        // operation. The no-pending case must not allocate an empty Message[]
        // on every single-part receive.
        if (!hasPending()) {
            frames = NO_FRAMES;
            index = 0;
            return;
        }
        for (int i = index; i < frames.length; i++) {
            if (frames[i] != null) {
                try {
                    frames[i].close();
                } catch (RuntimeException ignored) {
                }
            }
        }
        frames = NO_FRAMES;
        index = 0;
    }
}
