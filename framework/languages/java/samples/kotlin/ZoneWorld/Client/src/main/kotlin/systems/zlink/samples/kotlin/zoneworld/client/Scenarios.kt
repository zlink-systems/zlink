package systems.zlink.samples.kotlin.zoneworld.client

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.samples.kotlin.zoneworld.shared.Messages
import systems.zlink.samples.kotlin.zoneworld.shared.ZoneWorldSpec

internal typealias Scenario = suspend (ClientOptions) -> Unit

internal object Scenarios {
    private val topologySettleTimeout = Duration.ofSeconds(5)

    val clientDriven: Map<String, Scenario> = linkedMapOf(
        "ZW-A1" to ::a1, "ZW-A2" to ::a2, "ZW-A3" to ::a3, "ZW-A4" to ::a4,
        "ZW-A5" to ::a5, "ZW-B1" to ::b1, "ZW-B2" to ::b2, "ZW-B3" to ::b3,
        "ZW-B5" to ::b5, "ZW-B6" to ::b6, "ZW-B7" to ::b7, "ZW-C1" to ::c1,
        "ZW-C4" to ::c4, "ZW-D1" to ::d1, "ZW-E1" to ::e1, "ZW-E2" to ::e2,
        "ZW-E3" to ::e3, "ZW-E4" to ::e4, "ZW-E6" to ::e6, "ZW-F1" to ::f1,
        "ZW-F3" to ::f3, "ZW-F4" to ::f4,
    )
    val runnerDriven: Map<String, Scenario> = linkedMapOf(
        "ZW-B4" to ::b4, "ZW-B8" to ::b8, "ZW-C2" to ::c2, "ZW-C3" to ::c3,
        "ZW-E5-arm" to ::e5Arm, "ZW-E5" to ::e5, "ZW-G2" to ::g2,
        "ZW-G4" to ::g4, "ZW-G4-fresh" to ::g4Fresh,
    )

    private suspend fun a1(options: ClientOptions) {
        val player = Game.create(options, unique("a1"))
        withResources(player) {
            val join = player.join()
            ensure(join.error == null, "target admission completes before JoinWorldRes")
            ensure(join.zoneId == "zone-nw" && join.x == 25 && join.y == 25,
                "canonical spawn is zone-nw (25,25)")
        }
    }

    private suspend fun a2(options: ClientOptions) {
        val player = Game.create(options, unique("a2"))
        withResources(player) { player.join(); player.moveTo(28, 27) }
    }

    private suspend fun a3(options: ClientOptions) {
        val player = Game.create(options, unique("a3")); val probes = Probes.create(options); val ops = Ops.create(options)
        withResources(player, probes, ops) {
            player.join(); reject(player, -1, 25, "OutOfRange"); reject(player, 31, 25, "TooFar")
            player.moveTo(49, 49); reject(player, 50, 50, "DiagonalCrossing"); resetMaintenance(ops)
            val pair = probes.pair(); ensure(pair.error == null, "cross-owner pair exists")
            val crossing = edge(pair.sourceZoneId, pair.targetZoneId)
            player.moveTo(crossing.source.x, crossing.source.y)
            val node = nodeOwning(ops.watch(), pair.targetZoneId); ops.maintenance(node, true)
            try { reject(player, crossing.target.x, crossing.target.y, "ZoneMaintenance") }
            finally { ops.maintenance(node, false) }
        }
    }

    private suspend fun a4(options: ClientOptions) {
        val first = Game.create(options, unique("a4-b")); val second = Game.create(options, unique("a4-a"))
        withResources(first, second) {
            first.join(); second.join()
            for (client in listOf(first, second)) {
                val state = client.connector.waitFor<Messages.ZoneStateNotify>()
                    .where { has(it.payload(), first.playerId) && has(it.payload(), second.playerId) }
                    .timeout(Duration.ofSeconds(20)).await().payload()
                ensure(has(state, first.playerId) && has(state, second.playerId),
                    "both clients observe both same-zone players")
            }
        }
    }

    private suspend fun a5(options: ClientOptions) {
        val firstId = unique("a5-\uE000"); val secondId = unique("a5-\uD800\uDC00")
        val first = Game.create(options, firstId); val second = Game.create(options, secondId)
        withResources(first, second) {
            first.join(); second.join()
            val state = first.connector.waitFor<Messages.ZoneStateNotify>()
                .where { has(it.payload(), firstId) && has(it.payload(), secondId) }
                .timeout(Duration.ofSeconds(20)).await().payload()
            val ids = state.players.map { it.playerId }
            ensure(ids == ids.sortedWith(ZoneWorldSpec.utf8Order), "Players are UTF-8 byte ordered")
            val resident = state.players.filter { it.playerId == firstId }
            ensure(resident.size == 1 && state.zoneId == resident.single().zoneId,
                "resident value wins over border copy")
        }
    }

    private suspend fun b1(options: ClientOptions) {
        val west = Game.create(options, unique("b1-w")); val east = Game.create(options, unique("b1-e"))
        withResources(west, east) { coroutineScope {
            west.join(); east.join(); east.moveTo(55, 25)
            val visible = async(start = CoroutineStart.UNDISPATCHED) {
                east.connector.waitFor<Messages.ZoneStateNotify>()
                    .where { it.payload().zoneId == "zone-ne" && has(it.payload(), west.playerId) }
                    .timeout(Duration.ofSeconds(30)).await().payload()
            }
            west.moveTo(45, 45); val neighbor = visible.await()
            val view = neighbor.players.first { it.playerId == west.playerId }
            ensure(view.zoneId == "zone-nw" && view.x >= 40, "adjacent zone sees the border-band player")
            east.moveTo(55, 55)
            repeat(ZoneWorldSpec.BORDER_EXPIRY_TICKS * 2) {
                val state = east.connector.waitFor<Messages.ZoneStateNotify>()
                    .where { it.payload().zoneId == "zone-se" }.timeout(Duration.ofSeconds(30)).await().payload()
                ensure(!has(state, west.playerId), "diagonal zone never sees border snapshot")
            }
        } }
    }

    private suspend fun b2(options: ClientOptions) {
        val id = unique("b2"); val probes = Probes.create(options); val player = Game.create(options, id)
        withResources(probes, player) {
            val pair = requiredPair(probes); player.join(); player.moveTo(center(pair.sourceZoneId).x, center(pair.sourceZoneId).y)
            val before = probes.actor(id); player.moveTo(center(pair.targetZoneId).x, center(pair.targetZoneId).y)
            val after = probes.actor(id)
            ensure(before.ownerNodeRid == pair.sourceOwnerNodeRid && after.ownerNodeRid == pair.targetOwnerNodeRid &&
                before.ownerNodeRid != after.ownerNodeRid, "actor crosses owners")
            val target = center(pair.targetZoneId); player.moveTo(target.x + if (target.x < 50) 1 else -1, target.y)
        }
        val resumed = Game.create(options, id)
        withResources(resumed) { ensure(resumed.join().error == null, "relocated identity rebinds") }
    }

    private suspend fun b3(options: ClientOptions) {
        val id = unique("b3"); val probes = Probes.create(options); val player = Game.create(options, id)
        withResources(probes, player) {
            val pair = requiredPair(probes); player.join(); val source = center(pair.sourceZoneId); val target = center(pair.targetZoneId)
            player.moveTo(source.x, source.y); val before = probes.actor(id)
            player.moveTo(target.x, target.y); val after = probes.actor(id)
            ensure(before.actorId == after.actorId && before.objectGeneration == after.objectGeneration &&
                before.ownerNodeRid != after.ownerNodeRid, "relocation preserves actor identity and advances owner")
        }
    }

    private data class Follow(val probes: Probes, val player: Game, val generation: Long)
    private suspend fun preparedFollow(prefix: String, options: ClientOptions): Follow {
        val probes = Probes.create(options); val player = Game.create(options, unique(prefix))
        val pair = requiredPair(probes); player.join(); val source = center(pair.sourceZoneId); val target = center(pair.targetZoneId)
        player.moveTo(source.x, source.y); val before = probes.actor(player.playerId)
        val prime = "route-prime".toByteArray(StandardCharsets.UTF_8)
        val primed = probes.probe(player.playerId, "prime-${player.playerId}", prime)
        ensure(prime.contentEquals(primed.payload), "old owner route is primed")
        player.moveTo(target.x, target.y); val after = probes.actor(player.playerId)
        ensure(after.objectGeneration == before.objectGeneration && after.ownerNodeRid != before.ownerNodeRid,
            "probe actor relocated")
        return Follow(probes, player, after.objectGeneration)
    }

    private suspend fun b5(options: ClientOptions) {
        val value = preparedFollow("b5", options)
        withResources(value.probes, value.player) {
            val id = "one-way-${unique("b5")}"; value.probes.sendProbe(value.player.playerId, id,
                "one-way-payload".toByteArray(StandardCharsets.UTF_8))
            println("message-follow-one-way completed actor=${value.player.playerId} probe=$id generation=${value.generation}")
        }
    }

    private suspend fun b6(options: ClientOptions) {
        val value = preparedFollow("b6", options)
        withResources(value.probes, value.player) {
            val id = "request-${unique("b6")}"; val payload = "request-payload".toByteArray(StandardCharsets.UTF_8)
            val reply = value.probes.probe(value.player.playerId, id, payload)
            ensure(id == reply.probeId && payload.contentEquals(reply.payload), "followed request preserves payload and reply route")
            println("message-follow-request completed actor=${value.player.playerId} request=$id generation=${value.generation}")
        }
    }

    private suspend fun b7(options: ClientOptions) {
        val id = unique("b7"); val probes = Probes.create(options); val player = Game.create(options, id)
        withResources(probes, player) {
            val pair = requiredPair(probes); player.join(); val source = center(pair.sourceZoneId); val target = center(pair.targetZoneId)
            player.moveTo(source.x, source.y); val home = probes.actor(id)
            player.moveTo(target.x, target.y); val away = probes.actor(id)
            val returned = player.moveTo(source.x, source.y); player.moveTo(source.x, source.y + 3); val back = probes.actor(id)
            ensure(returned != null && returned.playerId == id && home.objectGeneration == away.objectGeneration &&
                home.objectGeneration == back.objectGeneration && home.ownerNodeRid == back.ownerNodeRid,
                "A-B-A identity and binding survive")
        }
    }

    private suspend fun c1(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            ensure(ops.watch().nodes.count { it.registered && it.connected } >= 2,
                "two ZoneNodes are independently registered and connected")
        }
    }

    private suspend fun c4(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) { coroutineScope {
            val alert = async(start = CoroutineStart.UNDISPATCHED) {
                ops.connector.waitFor<Messages.NodeAlertNotify>().where { it.payload().kind == "TimerHandlerFailed" }
                    .timeout(Duration.ofSeconds(40)).await().payload()
            }
            ops.watch(); val observed = alert.await()
            ensure(ops.watch().nodes.any { it.nodeId == observed.nodeId }, "timer failure alert names a current node")
        } }
    }

    private suspend fun d1(options: ClientOptions) {
        val players = mutableListOf<Game>()
        try { coroutineScope {
            ZoneWorldSpec.zones().forEach { zone ->
                val player = Game.create(options, unique("d1-$zone")); players += player
                player.join(); center(zone).also { player.moveTo(it.x, it.y) }
            }
            val waits = players.map { player -> async(start = CoroutineStart.UNDISPATCHED) {
                player.connector.waitFor<Messages.WorldAnnounceNotify>().timeout(REQUEST_TIMEOUT).await().payload()
            } }
            val ops = Ops.create(options); withResources(ops) {
                val published = ops.connector.request(Messages.AnnounceWorldReq("maintenance in 10 minutes"))
                    .timeout(REQUEST_TIMEOUT).awaitReply<Messages.AnnounceWorldRes>()
                waits.forEach { ensure(it.await().announcementId == published.announcementId,
                    "every zone client receives the same announcement") }
                players.forEach { it.connector.expectNone<Messages.WorldAnnounceNotify>("WorldAnnounceNotify")
                    .within(Duration.ofSeconds(3)).await() }
            }
        } } finally { players.reversed().forEach { runCatching { it.close() } } }
    }

    private suspend fun e1(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            resetMaintenance(ops); val before = ops.watch(); val target = before.nodes.filter { it.registered }.maxBy { it.nodeId }
            val others = before.nodes.filter { it.nodeId != target.nodeId }.associate { it.nodeId to it.maintenance }
            val set = ops.maintenance(target.nodeId, true)
            try {
                ensure(set.error == null && set.enabled, "target desired state is stored")
                val after = ops.watch(); ensure(after.nodes.first { it.nodeId == target.nodeId }.maintenance,
                    "target alone enters maintenance")
                others.forEach { (id, state) -> ensure(after.nodes.first { it.nodeId == id }.maintenance == state,
                    "non-target maintenance is unchanged") }
            } finally { ops.maintenance(target.nodeId, false) }
        }
    }

    private suspend fun e2(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            resetMaintenance(ops); val node = nodeOwning(ops.watch(), "zone-nw"); ops.maintenance(node, true)
            try { val player = Game.create(options, unique("e2")); withResources(player) {
                ensure(player.join().error == "ZoneMaintenance", "target OnActorJoin rejects a new entry")
            } } finally { ops.maintenance(node, false) }
        }
    }

    private suspend fun e3(options: ClientOptions) {
        val ops = Ops.create(options); val player = Game.create(options, unique("e3")); withResources(ops, player) {
            resetMaintenance(ops); player.join(); val node = nodeOwning(ops.watch(), "zone-nw"); ops.maintenance(node, true)
            try { player.moveTo(30, 30) } finally { ops.maintenance(node, false) }
        }
    }

    private suspend fun e4(options: ClientOptions) {
        val ops = Ops.create(options); val player = Game.create(options, unique("e4")); withResources(ops, player) {
            resetMaintenance(ops)
            val pairs = listOf(listOf("zone-nw", "zone-ne"), listOf("zone-nw", "zone-sw"),
                listOf("zone-ne", "zone-se"), listOf("zone-sw", "zone-se"))
            val deadline = System.nanoTime() + topologySettleTimeout.toNanos()
            var nodes: Messages.WatchNodesRes; var selected: List<String>?
            while (true) {
                val observed = ops.watch(); selected = pairs.firstOrNull { pair -> observed.nodes.any {
                    it.registered && it.zones.containsAll(pair) } }
                if (selected != null) { nodes = observed; break }
                ensure(System.nanoTime() < deadline,
                    "ZW-E4 requires two adjacent zones currently owned by the same ZoneNode")
                delay(100)
            }
            val pair = requireNotNull(selected); val node = nodes.nodes.first { it.registered && it.zones.containsAll(pair) }.nodeId
            val crossing = edge(pair[0], pair[1]); player.join(); player.moveTo(crossing.source.x, crossing.source.y)
            ops.maintenance(node, true)
            // The Ops push and the same-process Spot join run on independent framework turns.
            // Let the observed maintenance fanout finish applying before the intra-node join.
            delay(100)
            try { reject(player, crossing.target.x, crossing.target.y, "ZoneMaintenance") }
            finally { ops.maintenance(node, false) }
        }
    }

    private suspend fun e6(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            val target = ops.watch().nodes.first { it.registered }
            val result = ops.connector.request(Messages.NodeDiagnosticsReq(target.nodeId))
                .timeout(REQUEST_TIMEOUT).awaitReply<Messages.NodeDiagnosticsRes>()
            ensure(result.error == null && result.nodeId == target.nodeId && result.zones.isNotEmpty() && result.playerCount >= 0,
                "diagnostics returns current zones, count and maintenance")
        }
    }

    private suspend fun f1(options: ClientOptions) {
        val player = Game.create(options, unique("f1")); withResources(player) { coroutineScope {
            val first = async(start = CoroutineStart.UNDISPATCHED) {
                player.connector.waitFor<Messages.ZoneStateNotify>().where { it.payload().players.any(Messages.PlayerView::isBot) }
                    .timeout(Duration.ofSeconds(30)).await().payload()
            }
            player.join(); val bots = first.await().players.filter { it.isBot }.associate { it.playerId to Point(it.x, it.y) }
            player.connector.waitFor<Messages.ZoneStateNotify>().where { state -> state.payload().players.any {
                it.isBot && bots[it.playerId]?.let { old -> old.x != it.x || old.y != it.y } == true
            } }.timeout(Duration.ofSeconds(30)).await()
        } }
    }

    private suspend fun f3(options: ClientOptions) {
        val ops = Ops.create(options); val player = Game.create(options, unique("f3")); withResources(ops, player) {
            resetMaintenance(ops); player.join()
            val boundary = player.connector.waitFor<Messages.ZoneStateNotify>().where { aboutToCross(it.payload()) != null }
                .timeout(Duration.ofSeconds(45)).await().payload()
            val bot = requireNotNull(aboutToCross(boundary)); val targetZone = if (bot.zoneId == "zone-nw") "zone-ne" else "zone-nw"
            val node = nodeOwning(ops.watch(), targetZone); ops.maintenance(node, true)
            try {
                val initial = bot.x; val east = bot.zoneId == "zone-nw"
                val reversed = player.connector.waitFor<Messages.ZoneStateNotify>().where { state -> state.payload().players.any {
                    it.playerId == bot.playerId && if (east) it.x < initial else it.x > initial
                } }.timeout(Duration.ofSeconds(45)).await().payload()
                ensure(reversed.players.any { it.playerId == bot.playerId }, "rejected bot reverses direction")
            } finally { ops.maintenance(node, false) }
        }
    }

    private suspend fun f4(options: ClientOptions) {
        val player = Game.create(options, unique("f4")); val ops = Ops.create(options); withResources(player, ops) {
            player.join(); ops.connector.request(Messages.AnnounceWorldReq("bots receive nothing"))
                .timeout(REQUEST_TIMEOUT).awaitReply<Messages.AnnounceWorldRes>()
            reject(player, -40, player.y, "OutOfRange")
            val state = player.connector.waitFor<Messages.ZoneStateNotify>().where { it.payload().players.any(Messages.PlayerView::isBot) }
                .timeout(Duration.ofSeconds(20)).await().payload()
            ensure(state.players.any(Messages.PlayerView::isBot), "no-push traffic runs alongside bots")
        }
    }

    private suspend fun b4(options: ClientOptions) {
        val probes = Probes.create(options); val ops = Ops.create(options); withResources(probes, ops) { coroutineScope {
            val pair = requiredPair(probes); val crossing = edge(pair.sourceZoneId, pair.targetZoneId)
            val source = Game.create(options, unique("b4-source")); val target = Game.create(options, unique("b4-target"))
            withResources(source, target) {
                source.join(); target.join(); source.moveTo(crossing.source.x, crossing.source.y)
                val visible = async(start = CoroutineStart.UNDISPATCHED) {
                    source.connector.waitFor<Messages.ZoneStateNotify>().where {
                        it.payload().zoneId == pair.sourceZoneId && has(it.payload(), target.playerId)
                    }.timeout(Duration.ofSeconds(30)).await()
                }
                target.moveTo(crossing.target.x, crossing.target.y); visible.await()
                val node = nodeOwning(ops.watch(), pair.targetZoneId)
                val expired = async(start = CoroutineStart.UNDISPATCHED) {
                    source.connector.waitFor<Messages.ZoneStateNotify>().where {
                        it.payload().zoneId == pair.sourceZoneId && !has(it.payload(), target.playerId)
                    }.timeout(Duration.ofSeconds(60)).await()
                }
                println("scenario ZW-B4 armed node=$node"); expired.await()
            }
        } }
    }

    private suspend fun c2(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) { coroutineScope {
            val node = ops.watch().nodes.filter { it.connected }.maxBy { it.nodeId }.nodeId
            val dropped = async(start = CoroutineStart.UNDISPATCHED) {
                ops.connector.waitFor<Messages.NodeStatusNotify>().where { it.payload().nodeId == node && !it.payload().connected }
                    .timeout(Duration.ofSeconds(60)).await().payload()
            }
            println("scenario ZW-C2 armed node=$node"); ensure(!dropped.await().connected, "runtime event reports disconnect")
        } }
    }

    private suspend fun c3(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) { coroutineScope {
            val node = ops.watch().nodes.filter { it.registered }.maxBy { it.nodeId }.nodeId
            val expired = async(start = CoroutineStart.UNDISPATCHED) {
                ops.connector.waitFor<Messages.NodeStatusNotify>().where { it.payload().nodeId == node && !it.payload().registered }
                    .timeout(Duration.ofSeconds(60)).await().payload()
            }
            println("scenario ZW-C3 armed node=$node"); ensure(!expired.await().registered, "report expires after TTL")
        } }
    }

    private suspend fun e5Arm(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            val node = ops.watch().nodes.filter { it.registered }.maxBy { it.nodeId }.nodeId
            ensure(ops.maintenance(node, true).enabled, "maintenance is stored before restart")
            println("scenario ZW-E5-arm armed node=$node")
        }
    }

    private suspend fun e5(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            val node = ops.watch().nodes.first { it.registered && it.connected && it.maintenance }
            val diagnostics = ops.connector.request(Messages.NodeDiagnosticsReq(node.nodeId))
                .timeout(REQUEST_TIMEOUT).awaitReply<Messages.NodeDiagnosticsRes>()
            try { ensure(diagnostics.error == null && diagnostics.maintenance, "restart restores stored maintenance") }
            finally { ops.maintenance(node.nodeId, false) }
        }
    }

    private suspend fun g2(options: ClientOptions) {
        val ops = Ops.create(options); withResources(ops) {
            val node = ops.watch().nodes.first { it.registered && it.connected }; resetMaintenance(ops); ops.maintenance(node.nodeId, true)
            try {
                val diagnostics = ops.connector.request(Messages.NodeDiagnosticsReq(node.nodeId))
                    .timeout(REQUEST_TIMEOUT).awaitReply<Messages.NodeDiagnosticsRes>()
                ensure(diagnostics.error == null && diagnostics.nodeId == node.nodeId, "reverse-started node accepts operations")
            } finally { ops.maintenance(node.nodeId, false) }
        }
    }

    private suspend fun g4(options: ClientOptions) {
        val ops = Ops.create(options); val probes = Probes.create(options); withResources(ops, probes) { coroutineScope {
            val east = ops.watch().nodes.first { it.nodeId == "zone-node-2" }; val observed = requiredPair(probes)
            val pair = if (observed.targetZoneId in east.zones) observed else Messages.RelocationPairRes(
                observed.targetZoneId, observed.sourceZoneId, observed.targetOwnerNodeRid, observed.sourceOwnerNodeRid)
            val crossing = edge(pair.sourceZoneId, pair.targetZoneId); val player = Game.create(options, unique("g4-crash"))
            withResources(player) {
                player.join(); player.moveTo(crossing.source.x, crossing.source.y)
                val failed = async(start = CoroutineStart.UNDISPATCHED) {
                    player.connector.waitFor<Messages.CrashRelocationProbeRes>().where { it.payload().error == "Unavailable" }
                        .timeout(Duration.ofSeconds(60)).await().payload()
                }
                player.connector.send(Messages.CrashRelocationProbeMsg(crossing.target.x, crossing.target.y)).await()
                println("scenario ZW-G4 armed node=zone-node-2")
                ensure(failed.await().error == "Unavailable", "crashed Ready owner terminates in-flight operation Unavailable")
            }
        } }
    }

    private suspend fun g4Fresh(options: ClientOptions) {
        val probes = Probes.create(options); withResources(probes) {
            repeat(16) {
                val created = probes.fresh(unique("g4-fresh")); ensure(created.error == null && created.objectGeneration > 0,
                    "replacement accepts a fresh actor")
                println("scenario ZW-G4-fresh owner=${created.ownerNodeRid} actor=${created.actorId}")
            }
        }
    }

    private suspend fun b8(options: ClientOptions) {
        val probes = Probes.create(options); withResources(probes) {
            val pair = requiredPair(probes); val crossing = edge(pair.sourceZoneId, pair.targetZoneId); val id = unique("b8-seal")
            val player = Game.create(options, id); withResources(player) {
                player.join(); player.moveTo(crossing.source.x, crossing.source.y)
                val disconnected = CompletableFuture<String>()
                player.connector.onDisconnected { event ->
                    disconnected.complete(event.closeReason().toString()); CompletableFuture.completedFuture(null)
                }.use {
                    println("scenario ZW-B8 armed")
                    repeat(200) {
                        if (Files.exists(Path.of(options.faultArmFile))) return@repeat
                        delay(50)
                    }
                    ensure(Files.exists(Path.of(options.faultArmFile)), "runner did not arm command 44 block")
                    player.move(crossing.target.x, crossing.target.y)
                    println("scenario ZW-B8 disconnected reason=${disconnected.orTimeout(60, java.util.concurrent.TimeUnit.SECONDS).await()}")
                    player.connector.connect().await(); val rebound = player.join()
                    ensure(rebound.playerId == id && rebound.zoneId == pair.targetZoneId,
                        "reconnect rebinds the relocated actor")
                }
            }
        }
    }

    private suspend fun requiredPair(probes: Probes) = probes.pair().also {
        ensure(it.error == null, "probe discovers a cross-owner adjacent pair")
    }
    private fun nodeOwning(nodes: Messages.WatchNodesRes, zone: String) = nodes.nodes.first { zone in it.zones }.nodeId
    private suspend fun resetMaintenance(ops: Ops) { ops.watch().nodes.filter { it.registered }.forEach { ops.maintenance(it.nodeId, false) } }
    private suspend fun reject(player: Game, x: Int, y: Int, reason: String) = coroutineScope {
        val waiting = async(start = CoroutineStart.UNDISPATCHED) {
            player.connector.waitFor<Messages.MoveRejectedNotify>().timeout(Duration.ofSeconds(20)).await().payload()
        }
        val beforeX = player.x; val beforeY = player.y; player.move(x, y); val rejected = waiting.await()
        ensure(rejected.reason == reason && rejected.x == beforeX && rejected.y == beforeY,
            "rejection reason/order and unchanged coordinate: $reason")
    }
    private fun has(state: Messages.ZoneStateNotify, playerId: String) = state.players.any { it.playerId == playerId }
    private fun aboutToCross(state: Messages.ZoneStateNotify) = state.players.firstOrNull { value -> value.isBot &&
        value.playerId.endsWith("-x") && ((value.zoneId == "zone-nw" && value.x + ZoneWorldSpec.BOT_STEP >= 50) ||
            (value.zoneId == "zone-ne" && value.x - ZoneWorldSpec.BOT_STEP < 50)) }
}
