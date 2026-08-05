// SPDX-License-Identifier: MPL-2.0

package native

import "testing"

func TestTopicOutputStorageReusesBuffer(t *testing.T) {
	topic := &TopicMessage{}
	topic.topicBuf = reusableTopicBuffer(topic.topicBuf)
	first := &topic.topicBuf[0]
	topic.topicBuf = reusableTopicBuffer(topic.topicBuf)
	if got := &topic.topicBuf[0]; got != first {
		t.Fatalf("TopicMessage allocated a new topic buffer on reuse")
	}

	event := &SubscriptionEvent{}
	event.topicBuf = reusableTopicBuffer(event.topicBuf)
	first = &event.topicBuf[0]
	event.topicBuf = reusableTopicBuffer(event.topicBuf)
	if got := &event.topicBuf[0]; got != first {
		t.Fatalf("SubscriptionEvent allocated a new topic buffer on reuse")
	}
}
