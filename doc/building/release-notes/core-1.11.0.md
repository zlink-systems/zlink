[English](./core-1.11.0.md) | [한국어](./core-1.11.0.ko.md)

# libzlink 1.11.0 release notes

## Changes

- WRITABLE completion send_result values are ADMITTED 0, NOT_FOUND 801 (ENOENT), NOT_CONNECTED 802 (ENOTCONN, STREAM), and TIMED_OUT 803 (EAGAIN) (#1154).
- A DONTWAIT SEND or REQUEST waiting token ends with TIMED_OUT/EAGAIN if it has not completed by the SNDTIMEO deadline captured at submission. SNDTIMEO=-1 has no deadline (#1154).
- When all 65,536 completion reservations for a socket are in use, SEND DONTWAIT returns BACKPRESSURED/EAGAIN and ID 0, matching REQUEST. Previously it returned OUT_OF_MEMORY/ENOMEM (#1165).
- When one thread polls a socket and another sends or receives, command handling on the send/receive path no longer consumes the poller notification and leaves the poller asleep until another event. Only the poller consumes its notification (#1082).
- An immediately admitted send does not read SNDTIMEO or the clock (#1154).
