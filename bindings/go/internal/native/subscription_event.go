// SPDX-License-Identifier: MPL-2.0

package native

type SubscriptionEvent struct {
	routingID  RoutingID
	subscribed bool
	topic      string
	topicBuf   []byte
}

func (s SubscriptionEvent) RoutingID() RoutingID {
	return s.routingID
}

func (s SubscriptionEvent) HasRoutingID() bool {
	return s.routingID.Size() > 0
}

func (s SubscriptionEvent) Subscribed() bool {
	return s.subscribed
}

func (s SubscriptionEvent) Topic() string {
	return s.topic
}
