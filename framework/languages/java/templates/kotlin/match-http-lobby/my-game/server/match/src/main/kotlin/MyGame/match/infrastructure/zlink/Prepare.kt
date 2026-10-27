package MyGame.match.infrastructure.zlink

import MyGame.*
import systems.zlink.framework.handlers.*

@ZLinkHandlerGroup(Names.ROOM)
class Prepare {
    @ZLinkSpotRequest
    suspend fun prepare(room: Room, request: PrepareRoom): RoomPrepared = room.prepare(request)
}
