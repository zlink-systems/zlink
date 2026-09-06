// SPDX-License-Identifier: MPL-2.0
'use strict';

// The DTO and packet name the `zlink-framework-<lang>` row exchanges.
//
// spec section 3: gRPC and the ZLink framework use the same protobuf DTO. `.NET` registers
// the request and the send handler under the same packet name `BenchPayload`
// (ZLinkServer/Program.cs), so node does the same and the two rows stay
// comparable.

const PACKET_NAME = 'BenchPayload';
const CHANNEL_NAME = 'bench';
const MESH_NAME = 'bench';
const SERVER_ROUTING_ID = 'bench-server';
const CLIENT_ROUTING_ID = 'bench-client';

class BenchPayload {
  constructor(body) {
    this.body = body;
  }
}

module.exports = {
  PACKET_NAME,
  CHANNEL_NAME,
  MESH_NAME,
  SERVER_ROUTING_ID,
  CLIENT_ROUTING_ID,
  BenchPayload
};
