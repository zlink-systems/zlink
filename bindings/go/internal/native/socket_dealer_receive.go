// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

// Recv uses the DEALER typed receive substrate so request records retain their
// request sequence and can be answered through Received.Reply. The message
// type remains an internal routing decision; ordinary callers only see the
// common Received envelope.
func (s *DealerSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if s.connectionSocket.hasReceiveHandler() {
		return false, &RecvError{Result: RecvBusy, nativeErrno: int(C.EBUSY)}
	}

	reuse := out.beginReceive()
	var messageType C.uint8_t
	var requestSeq C.uint64_t
	parts, err := recvMultipart(reuse, flags, func(part *C.zlink_msg_t, hasMore *C.zlink_part_flag_t, recvFlags C.zlink_recv_flags_t) error {
		return recvErrorFromResult(C.zlink_dealer_recv_part(
			s.raw(),
			&messageType,
			&requestSeq,
			part,
			hasMore,
			recvFlags,
		))
	})
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}

	seq := uint64(requestSeq)
	var reply func(SendFlags, []*Message) error
	if messageType == C.ZLINK_DEALER_MESSAGE_REQUEST && seq != 0 {
		reply = receivedReplyToDealer(s.reply, seq)
	}
	out.replace(RoutingID{}, parts, seq, seq != 0, reply, nil)
	return true, nil
}

func (s *DealerSocket) reply(requestSeq uint64, flags SendFlags, parts ...*Message) error {
	if requestSeq == 0 {
		return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if err := validateReplyFlags(flags); err != nil {
		return err
	}
	return submitMultipartFromClones(parts, true, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_dealer_reply_part(
			s.raw(),
			C.uint64_t(requestSeq),
			part,
			partFlag,
		))
	})
}
