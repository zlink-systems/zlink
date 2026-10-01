"""The scenario, role-kind and CLI-option tables (perf/README.ko.md §5, §6.3, §10, §11, §15.1).

Every decision about which roles a scenario needs, which options it consumes, which values it accepts and which
cells a matrix expands to is made here and nowhere else; the runner only reads these tables.
"""
from __future__ import annotations

from dataclasses import dataclass
import itertools
from typing import Callable

# Role kind -> role executable (§6.3). A language launcher maps the executable name to its own project (§17).
ROLE_KINDS = {
    "session-actor-local": "SessionActorLocalServer",
    "session": "SessionServer",
    "actor": "ActorServer",
    "channel": "ChannelServer",
    "spot": "SpotServer",
    "actor-caller": "ActorCallerServer",
    "publisher": "PublisherServer",
    "subscriber": "SubscriberServer",
}
CLIENT = "Client"
EXECUTABLES = (CLIENT, *ROLE_KINDS.values())


@dataclass(frozen=True)
class Listen:
    """One transport listener a role process owns. key `topology` resolves to `mesh` or `clientserver` per cell."""
    key: str  # stream | mesh | topology
    when: str | None = None  # only for this channel topology


STREAM, MESH, TOPOLOGY = Listen("stream"), Listen("mesh"), Listen("topology")


@dataclass(frozen=True)
class RoleUse:
    """A role process a scenario starts. Table order is the start order (receivers before the source)."""
    kind: str
    object_role: str = "None"  # None | ObjectServer | ObjectClient
    source: bool = False  # owns the server-driven load loop
    instance: int = 0
    count: str | None = None  # "subscribers": one process per --subscriber-count, roleInstance = subscriberId
    listens: tuple[Listen, ...] = ()
    peer: bool = False  # manually connects to the first non-source role's first listener
    objects: str | None = None  # spot | actor: this role's config and manifest carry the cell's IDs


@dataclass(frozen=True)
class Scenario:
    name: str
    family: str  # CS | S2S | Spot | AC | PS | Baseline
    section: str
    driver: str  # clients: CS client processes own the loop; source: the `source` role owns it
    roles: tuple[RoleUse, ...]
    payload: int  # representative payload; 1024 and 4096 both run in a matrix
    modes: tuple[str, ...]
    store: bool  # needs the run's Docker Redis (§20)
    discovery: str  # none | manual | automatic
    execution: str
    terminals: tuple[str, ...] = ("ordinary",)  # first = default; more than one = matrix axis
    topologies: tuple[str, ...] = ()  # empty = not applicable ("na"); more than one = matrix axis
    spot_axis: tuple[int, ...] = ()  # matrix values of --spot-count (§10.5)
    channel: bool = False  # roles share a ChannelName
    worker: bool = False
    references: tuple[str, ...] = ()  # names that point at this cell but are not scenarios (§11.3)
    owner_kinds: tuple[str, ...] = ()  # role kinds whose originals are the primary owners (§15.4); empty = clients or the source role
    aggregation: str = "echo"  # echo | fanout-sequences: how the owners' originals combine into the result (§15.4)

    def uses(self, **fields) -> bool:
        return any(all(getattr(role, key) == value for key, value in fields.items()) for role in self.roles)

    @property
    def object_roles(self) -> str:
        return "+".join(sorted({role.object_role for role in self.roles} - {"None"})) or "None"


@dataclass(frozen=True)
class Option:
    """A numeric or path option of §5 with the scenarios that consume it. Choice options live on Scenario."""
    default: int | None
    consumed_by: Callable[[Scenario], bool]
    kind: str = "int"  # int | path
    note: str = ""


OPTIONS = {
    "connections": Option(10000, lambda s: s.driver == "clients"),
    "logical_streams": Option(10000, lambda s: s.driver == "source"),
    "client_count": Option(1, lambda s: True),
    "inflight": Option(1, lambda s: True),
    "connect_concurrency": Option(256, lambda s: True),  # §5: CS connector setup, and a source role's object preparation and probes
    "spot_count": Option(16, lambda s: s.uses(objects="spot")),
    "subscriber_count": Option(8, lambda s: s.uses(count="subscribers")),
    "worker_task_millis": Option(5, lambda s: s.worker),
    "worker_pool_size": Option(8, lambda s: s.worker),
}
# Choice options: option key -> Scenario field holding the accepted values (empty = not applicable).
CHOICES = {"mode": "modes", "terminal": "terminals", "channel_topology": "topologies"}
MODE_VALUES = ("request", "send-send", "no-await", "worker-offload", "publish")
TERMINAL_VALUES = ("ordinary", "yield")
TOPOLOGY_VALUES = ("routemesh", "clientserver")
PAYLOADS = (1024, 4096)

SCENARIOS = (
    Scenario("cs-local-session-actor-echo", "CS", "10.1", "clients",
             (RoleUse("session-actor-local", "ObjectServer", listens=(STREAM, MESH), objects="actor"),),
             1024, ("request",), True, "automatic", "Framework default"),
    Scenario("cs-remote-session-actor-echo", "CS", "10.2", "clients",
             (RoleUse("actor", "ObjectServer", listens=(MESH,), objects="actor"),
              RoleUse("session", "ObjectClient", listens=(STREAM, MESH), objects="actor")),
             1024, ("request",), True, "automatic", "Framework default"),
    Scenario("s2s-channel-to-spot-request-echo", "S2S", "10.3", "source",
             (RoleUse("spot", "ObjectServer", listens=(MESH,), objects="spot"),
              RoleUse("channel", "ObjectClient", source=True, listens=(MESH,), objects="spot")),
             4096, ("request",), True, "automatic", "SpotWide"),
    Scenario("s2s-channel-to-spot-send-send-echo", "S2S", "10.4", "source",
             (RoleUse("spot", "ObjectServer", listens=(MESH,), objects="spot"),
              RoleUse("channel", "ObjectClient", source=True, listens=(MESH,), objects="spot")),
             4096, ("send-send",), True, "automatic", "SpotWide", topologies=("routemesh",), channel=True),
    Scenario("s2s-spot-to-channel-request-echo", "S2S", "10.5", "source",
             (RoleUse("channel", listens=(MESH,)),
              RoleUse("spot", "ObjectServer", source=True, listens=(MESH,), objects="spot")),
             4096, ("request",), True, "automatic", "SpotWide", terminals=("ordinary", "yield"),
             topologies=("routemesh",), spot_axis=(1, 16), channel=True),
    Scenario("s2s-spot-to-channel-send-send-echo", "S2S", "10.6", "source",
             (RoleUse("channel", "ObjectClient", listens=(MESH,), objects="spot"),
              RoleUse("spot", "ObjectServer", source=True, listens=(MESH,), objects="spot")),
             4096, ("send-send",), True, "automatic", "SpotWide", topologies=("routemesh",), channel=True),
    Scenario("spot-no-await-echo", "Spot", "10.7", "source",
             (RoleUse("spot", "ObjectServer", source=True, listens=(MESH,), objects="spot"),),
             1024, ("no-await",), True, "automatic", "SpotWide", references=("spot-local-echo",)),
    Scenario("spot-worker-offload-echo", "Spot", "10.8", "source",
             (RoleUse("spot", "ObjectServer", source=True, listens=(MESH,), objects="spot"),),
             1024, ("worker-offload",), True, "automatic", "SpotWide", terminals=("yield", "ordinary"), worker=True),
    Scenario("actor-no-bind-request-echo", "AC", "10.9", "source",
             (RoleUse("actor", "ObjectServer", listens=(MESH,), objects="actor"),
              RoleUse("actor-caller", "ObjectClient", source=True, listens=(MESH,), objects="actor")),
             4096, ("request",), True, "automatic", "Framework default"),
    Scenario("actor-no-bind-send-send-echo", "AC", "10.10", "source",
             (RoleUse("actor", "ObjectServer", listens=(MESH,), objects="actor"),
              RoleUse("actor-caller", "ObjectClient", source=True, listens=(MESH,), objects="actor")),
             4096, ("send-send",), True, "automatic", "Framework default", topologies=("routemesh",), channel=True),
    Scenario("pubsub-fanout-echo", "PS", "10.11", "source",
             (RoleUse("subscriber", count="subscribers"),  # endpoint-less automatic subscriber: discovers the publisher in the Store
              RoleUse("publisher", source=True, listens=(Listen("fanout"),))),  # Classic fanout is its own listener, not a MeshNode
             1024, ("publish",), True, "automatic", "Framework default", channel=True,
             owner_kinds=("publisher", "subscriber"), aggregation="fanout-sequences"),
    Scenario("session-echo-only", "Baseline", "11.1", "clients",
             (RoleUse("session", listens=(STREAM,)),),
             1024, ("request",), False, "none", "Immediate"),
    Scenario("channel-echo-only", "Baseline", "11.2", "source",
             (RoleUse("channel", instance=1, listens=(TOPOLOGY,)),
              RoleUse("channel", source=True, listens=(Listen("mesh", "routemesh"),), peer=True)),
             4096, ("request",), False, "manual", "Framework default", topologies=TOPOLOGY_VALUES, channel=True),
)
BY_NAME = {scenario.name: scenario for scenario in SCENARIOS}
assert len(BY_NAME) == len(SCENARIOS) == 13
assert all(role.kind in ROLE_KINDS for scenario in SCENARIOS for role in scenario.roles)


def source_role(scenario: Scenario) -> RoleUse | None:
    return next((role for role in scenario.roles if role.source), None)


def instances(role: RoleUse, subscriber_count: int | None) -> range:
    return range(subscriber_count) if role.count == "subscribers" else range(role.instance, role.instance + 1)


def owner_files(scenario: Scenario, client_files: list[str], subscriber_count: int | None) -> list[str]:
    """The original files whose metrics are the cell's primary result (§15.4), in the order the aggregation reads them."""
    if scenario.owner_kinds:
        return [f"server-{role.kind}-{index}.json" for kind in scenario.owner_kinds
                for role in scenario.roles if role.kind == kind for index in instances(role, subscriber_count)]
    if scenario.driver == "clients":
        return list(client_files)
    return [f"server-{source_role(scenario).kind}-{source_role(scenario).instance}.json"]


@dataclass(frozen=True)
class Cell:
    """One measured configuration; every field but the payload is a §15.1 variant part."""
    scenario: Scenario
    payload: int
    mode: str
    terminal: str
    topology: str | None
    spot_count: int | None
    subscriber_count: int | None

    def variant(self, config_hash: str) -> str:
        def part(value): return "na" if value is None else value
        return f"{self.mode}-{self.terminal}-{part(self.topology)}-s{part(self.spot_count)}-n{part(self.subscriber_count)}-{config_hash}"

    def cell_id(self, config_hash: str) -> str:
        return f"{self.scenario.name}/{self.payload}/{self.variant(config_hash)}"


def selected(args) -> list[Scenario]:
    return [BY_NAME[args.scenario]] if args.scenario else list(SCENARIOS)


def values(scenario: Scenario, args) -> dict:
    """The numeric options this scenario consumes, defaults applied. Options it does not consume are absent."""
    return {key: (getattr(args, key) if getattr(args, key) is not None else option.default)
            for key, option in OPTIONS.items() if option.consumed_by(scenario)}


def check(args, matrix: bool) -> str | None:
    """The first preflight problem in the explicitly given options (§5), or None."""
    scenarios = selected(args)
    flag = lambda key: "--" + key.replace("_", "-")  # noqa: E731
    for key, option in OPTIONS.items():
        if getattr(args, key) is not None and not any(option.consumed_by(s) for s in scenarios):
            return f"{flag(key)} is not applicable: {option.note or 'no selected scenario consumes it'}"
    if args.client_count not in (None, 1) and any(s.driver == "source" for s in scenarios):
        return "server-driven cells require client-count=1"
    for key, field in CHOICES.items():
        value = getattr(args, key)
        if value is None:
            continue
        if matrix:
            return f"run_perf.sh fixes or expands {flag(key)} per scenario; select one cell with run_single.sh"
        accepted = getattr(scenarios[0], field)
        if value not in accepted:
            return f"{flag(key)} {value} is not accepted by {scenarios[0].name}: {', '.join(accepted) or 'not applicable'}"
    if matrix and args.spot_count is not None and any(s.spot_axis for s in scenarios):
        return "run_perf.sh expands --spot-count for a scenario with a SpotId axis; select one cell with run_single.sh"
    return None


def expand(args, matrix: bool) -> list[Cell]:
    """The cells to run: a matrix expands every axis and both payloads; a single run is one cell per scenario."""
    cells = []
    for scenario in selected(args):
        consumed = values(scenario, args)
        payloads = args.payloads if matrix else [args.payload_size or scenario.payload]
        terminals = scenario.terminals if matrix else [args.terminal or scenario.terminals[0]]
        topologies = ([None] if not scenario.topologies else
                      scenario.topologies if matrix else [args.channel_topology or scenario.topologies[0]])
        spots = scenario.spot_axis if matrix and scenario.spot_axis else [consumed.get("spot_count")]
        for payload, terminal, topology, spot in itertools.product(payloads, terminals, topologies, spots):
            cells.append(Cell(scenario, payload, scenario.modes[0], terminal, topology, spot, consumed.get("subscriber_count")))
    return cells
