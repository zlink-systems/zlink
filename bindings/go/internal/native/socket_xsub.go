// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

type XSubSocket struct {
	*subscribeSocket
}

func newXSubSocket(ctx *Context) (*XSubSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_XSUB)
	if err != nil {
		return nil, err
	}
	return &XSubSocket{
		subscribeSocket: &subscribeSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *XSubSocket) SubscriptionAt(index int) (string, bool, error) {
	return subscriptionAt(s.raw(), index)
}

func (s *XSubSocket) TopicsCount() (int, error) {
	return s.connectionSocket.getSubIntOption(C.ZLINK_SUB_OPT_TOPICS_COUNT)
}
