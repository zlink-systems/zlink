/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;

final class RequestSubmitLoop {
    private RequestSubmitLoop() {
    }

    static void submitErrnoParts(List<Message> parts,
                                 PartSubmitter submitter,
                                 SubmitFailure submitFailure) {
        submitParts(parts, submitter, rc -> {
            if (rc != 0) {
                throw submitFailure.fromLastError();
            }
        });
    }

    private static void submitParts(List<Message> parts,
                                    PartSubmitter submitter,
                                    ResultHandler resultHandler) {
        for (int i = 0; i < parts.size(); i++) {
            int partFlag = i + 1 < parts.size()
                ? Native.PART_MORE : Native.PART_FINAL;
            Message part = parts.get(i);
            int rc = NativeErrno.retryWhileInterrupted(
                () -> submitter.submit(part, partFlag),
                result -> result != 0);
            resultHandler.handle(rc);
        }
    }

    @FunctionalInterface
    interface PartSubmitter {
        int submit(Message part, int partFlag);
    }

    @FunctionalInterface
    interface SubmitFailure {
        RuntimeException fromLastError();
    }

    @FunctionalInterface
    private interface ResultHandler {
        void handle(int rc);
    }
}
