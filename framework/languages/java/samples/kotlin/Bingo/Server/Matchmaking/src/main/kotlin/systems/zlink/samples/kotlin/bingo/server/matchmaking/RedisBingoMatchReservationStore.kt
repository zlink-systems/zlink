package systems.zlink.samples.kotlin.bingo.server.matchmaking

import io.lettuce.core.RedisClient
import io.lettuce.core.ScriptOutputType
import java.util.Base64
import java.util.UUID
import java.util.concurrent.CompletionStage
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomSettingsPayload
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ReserveBingoRoomRes

class RedisBingoMatchReservationStore(topology: SampleTopology) : AutoCloseable {
    private val keyPrefix = topology.redisKeyPrefix
    private val client = RedisClient.create(
        if ("://" in topology.redisEndpoint) {
            topology.redisEndpoint
        } else {
            "redis://${topology.redisEndpoint}"
        },
    )
    private val connection = client.connect()

    fun reserve(request: ReserveBingoRoomReq): CompletionStage<ReserveBingoRoomRes> {
        require(request.mode == "two-player") { "unsupported bingo mode: ${request.mode}" }
        require(request.actorId.isNotBlank() && request.levelBucket.isNotBlank()) {
            "actor id and level bucket are required"
        }
        val roomId = "bingo-room-${UUID.randomUUID().toString().replace("-", "")}"
        val settings = BingoRoomSettingsPayload.newBuilder()
            .setRoomName("Bingo Room ${roomId.takeLast(6)}")
            .setMode(request.mode)
            .setRequiredPlayers(2)
            .setMaxDrawNumber(15)
            .setPurpose("Game")
            .build()
        val encoded = Base64.getEncoder().encodeToString(settings.toByteArray())
        return connection.async().eval<List<Any>>(
            SCRIPT,
            ScriptOutputType.MULTI,
            arrayOf("${keyPrefix}match:${request.levelBucket}:${request.mode}"),
            request.actorId,
            roomId,
            encoded,
            "2",
        ).thenApply { result ->
            val selected = BingoRoomSettingsPayload.parseFrom(
                Base64.getDecoder().decode(result[1].toString()),
            )
            ReserveBingoRoomRes.newBuilder()
                .setRoomId(result[0].toString())
                .setSettings(selected)
                .build()
        }
    }

    override fun close() {
        connection.close()
        client.shutdown()
    }

    companion object {
        private const val SCRIPT = """
            local key = KEYS[1]
            local actor = ARGV[1]
            local room = ARGV[2]
            local settings = ARGV[3]
            local required = tonumber(ARGV[4])
            local current = redis.call('HGET', key, 'RoomId')
            if not current then
              redis.call('HMSET', key, 'RoomId', room, 'Settings', settings,
                'ReservedActorIds', actor, 'RequiredPlayers', required)
              return {room, settings}
            end
            local actors = redis.call('HGET', key, 'ReservedActorIds') or ''
            local currentSettings = redis.call('HGET', key, 'Settings')
            if string.find('|' .. actors .. '|', '|' .. actor .. '|', 1, true) then
              return {current, currentSettings}
            end
            local count = 0
            for _ in string.gmatch(actors, '[^|]+') do count = count + 1 end
            if count >= required then
              redis.call('HMSET', key, 'RoomId', room, 'Settings', settings,
                'ReservedActorIds', actor, 'RequiredPlayers', required)
              return {room, settings}
            end
            redis.call('HSET', key, 'ReservedActorIds', actors .. '|' .. actor)
            return {current, currentSettings}
        """
    }
}
