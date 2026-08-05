package main

import (
	"bytes"
	"context"
	"fmt"
	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/samples/internal/samplecommon"
)

func main() {
	// --8<-- [start:doc]
	ctx, err := zlink.NewContext()
	samplecommon.Must(err)
	defer ctx.Close()

	publisher, err := ctx.XPubSocket()
	samplecommon.Must(err)
	defer publisher.Close()
	subscriber, err := ctx.SubSocket()
	samplecommon.Must(err)
	defer subscriber.Close()

	pubMon := samplecommon.OpenMonitor(publisher)
	defer pubMon.Close()
	subMon := samplecommon.OpenMonitor(subscriber)
	defer subMon.Close()

	endpoint := samplecommon.UniqueTCP("pubsub-recv")
	samplecommon.Must(publisher.Bind(endpoint))
	samplecommon.Must(subscriber.Connect(endpoint))
	samplecommon.WaitConnected(pubMon, subMon)

	topic := "prices"
	samplecommon.Must(subscriber.SetSubscription(topic))
	var event zlink.SubscriptionEvent
	_, err = publisher.ReceiveSubscriptionEvent(&event, zlink.RecvFlagsNone)
	samplecommon.Must(err)
	if !event.Subscribed() || event.Topic() != topic {
		samplecommon.Must(fmt.Errorf("unexpected subscription event"))
	}

	payload := "101.25"
	_, err = publisher.Publish(topic).Message(samplecommon.Message(payload)).Submit(context.Background())
	samplecommon.Must(err)

	var message zlink.TopicMessage
	_, err = subscriber.Subscribe(&message, zlink.RecvFlagsNone)
	samplecommon.Must(err)
	defer message.Close()
	part, err := message.SinglePartOrError()
	samplecommon.Must(err)
	if !bytes.Equal(part.Data(), []byte(payload)) {
		samplecommon.Must(fmt.Errorf("unexpected payload %q", string(part.Data())))
	}

	fmt.Printf("[pubsub/recv] publish: %q -> subscribe: %q\n", topic+"/"+payload, message.Topic()+"/"+string(part.Data()))
	// --8<-- [end:doc]
}
