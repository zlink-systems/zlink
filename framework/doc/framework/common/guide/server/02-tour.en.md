# Interactive Tour

!!! info "What you get from this chapter"

    You can see how ZLink's channel configurations (ClientServer · Fanout · RouteMesh), Spots and
    Actors, relocation without downtime, and backpressure behave by stepping through them yourself.

This is a browser simulation that walks through each feature step by step, using a single MORPG as
the example. Pick a feature in the tabs at the top and press the button for each step; the game view
and the message flow inside the servers move together. It does not use a real network, and its
timings and numbers are for demonstration only.

<iframe class="zlink-diagram" src="/common/diagrams/zlink-tour-en.html" title="ZLink interactive tour — the features through a MORPG" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/zlink-tour-en.html" target="_blank">↗ Open full size</a></p>

| Tab | What it shows | Details |
| --- | --- | --- |
| ClientServer | The API server calls feature servers by channel name and splits the load by weight | [Channel Messaging](20-channel-messaging.en.md) |
| Fanout | One publish from the ops tool reaches only the servers subscribed to that topic | [Channel Messaging](20-channel-messaging.en.md) |
| Lobby and dungeon · RouteMesh | From connecting to the town, entering a dungeon, the boss fight, reconnecting, and returning | [Spot](21-spot.en.md) · [Actor](22-actor.en.md) · [Session and Actor](24-actor-session.en.md) |
| Instance Spot | Several servers contribute to the same guild at once, and the guild processes them one at a time | [Spot](21-spot.en.md) · [Execution Model](32-execution-model.en.md) |
| Host Relocation | A server is taken down for maintenance mid-fight, and the room moves to another server and keeps running | [Relocation](37-relocation.en.md) |
| Backpressure | The same burst against a server that queues without limit and a ZLink server | [Backpressure](33-backpressure.en.md) |
