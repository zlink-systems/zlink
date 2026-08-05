'use strict';

module.exports = {
  ...require('../../../packages/stream-connector/dist/Runtime/Protocol/ZlinkStreamFrameCodec'),
  ...require('../../../packages/stream-connector/dist/Runtime/Protocol/ZlinkStreamHeaderCodec')
};
