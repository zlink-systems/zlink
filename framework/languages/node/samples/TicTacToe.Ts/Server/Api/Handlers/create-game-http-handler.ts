import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Configuration/sample-settings';
import { TICTACTOE_SAMPLE_CONFIG } from '../../Configuration/sample-config';
import { TicTacToeGameCreateReq } from '../../../Shared/Contracts/messages';
import type { ZLinkSpotManager } from '@zlink-systems/framework';
import type { TicTacToeSampleConfig } from '../../Configuration/sample-config';
import type {
  CreateGameHttpRes,
  CreateGameHttpReq
} from '../../../Shared/Contracts/messages';

@Injectable()
class CreateGameEndpoint {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
    @Inject(TICTACTOE_SAMPLE_CONFIG) private readonly config: TicTacToeSampleConfig
  ) {}

  async handle(request: CreateGameHttpReq): Promise<CreateGameHttpRes> {
    const gameName = request.gameName ?? 'match';
    // --8<-- [start:doc-create]
    const created = await this.spots
      .create(SampleNames.gameSpotType)                  // 이 stable type을 등록한 node가 후보가 된다.
      .inMesh(SampleNames.playSpotNode)                  // Spot을 만들 mesh를 고른다.
      .request(new TicTacToeGameCreateReq(gameName, 3))  // 새 Spot의 생성 callback에 전달할 최초 설정이다.
      .submit();                                         // Node의 비동기 완료 terminal이다.
    // --8<-- [end:doc-create]
    return {
      roomId: String(created.spot.spotId),
      gameName,
      playEndpoints: this.config.playEndpoints,
      playNodes: this.config.playEndpoints.map((streamEndpoint) => ({ streamEndpoint })),
      requiredLevel: 3
    };
  }
}

export { CreateGameEndpoint };
