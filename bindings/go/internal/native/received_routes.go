// SPDX-License-Identifier: MPL-2.0

package native

func receivedReplyToRouter(
	reply func(RoutingID, uint64, SendFlags, ...*Message) error,
	routingID RoutingID,
	requestSeq uint64,
) func(SendFlags, []*Message) error {
	return func(flags SendFlags, parts []*Message) error {
		return reply(routingID, requestSeq, flags, parts...)
	}
}

func receivedReplyToDealer(
	reply func(uint64, SendFlags, ...*Message) error,
	requestSeq uint64,
) func(SendFlags, []*Message) error {
	return func(flags SendFlags, parts []*Message) error {
		return reply(requestSeq, flags, parts...)
	}
}
