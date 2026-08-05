use crate::routing_id::RoutingId;

/// A subscriber's subscribe or unsubscribe as observed by an XPUB socket.
pub struct SubscriptionEvent {
    /// The subscriber's routing id, when known.
    routing_id: Option<RoutingId>,
    /// The topic that was subscribed or unsubscribed.
    topic: smol_str::SmolStr,
    /// `true` for a subscribe, `false` for an unsubscribe.
    subscribed: bool,
}

impl SubscriptionEvent {
    /// Creates an empty reusable event; reuse it across
    /// `receive_subscription_event` calls.
    pub fn empty() -> Self {
        Self {
            routing_id: None,
            topic: smol_str::SmolStr::default(),
            subscribed: false,
        }
    }

    pub(crate) fn new(
        routing_id: Option<RoutingId>,
        subscribed: bool,
        topic: smol_str::SmolStr,
    ) -> Self {
        Self {
            routing_id,
            topic,
            subscribed,
        }
    }

    pub(crate) fn replace_from(&mut self, source: SubscriptionEvent) {
        self.routing_id = source.routing_id;
        self.topic = source.topic;
        self.subscribed = source.subscribed;
    }

    /// Returns the subscriber's routing id, when present.
    pub fn routing_id(&self) -> Option<&RoutingId> {
        self.routing_id.as_ref()
    }

    /// Returns the subscription topic.
    pub fn topic(&self) -> &str {
        self.topic.as_str()
    }

    /// Returns `true` for a subscribe event and `false` for an unsubscribe.
    pub fn is_subscribed(&self) -> bool {
        self.subscribed
    }
}
