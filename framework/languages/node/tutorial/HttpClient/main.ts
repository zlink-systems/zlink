import { ZLinkHttpClient } from '@zlink-systems/http-client';

interface PlayerProfile {
  readonly playerId: string;
  readonly nickname: string;
  readonly level: number;
}

interface PlayerInfo {
  readonly playerId: string;
  readonly nickname: string;
}

interface RoomState {
  readonly title: string;
  readonly chat: readonly string[];
}

interface WeightResult {
  readonly channel: string;
  readonly weight: number;
}

interface ImportedResult {
  readonly imported: number;
}

const clientBaseUrl = 'http://127.0.0.1:5480';
const adminBaseUrl = 'http://127.0.0.1:5481';
const encoder = new TextEncoder();

// The tutorial keeps the examples linear so each call shows the public API that performs it.
async function main(): Promise<void> {
  // --8<-- [start:http-client-create]
  const client = ZLinkHttpClient.create(clientBaseUrl)
    .timeout(3000)
    .build();
  // --8<-- [end:http-client-create]

  try {
    // --8<-- [start:http-first-request]
    const first = await client
      .get('/players/p1/profile')
      .async<PlayerProfile>();
    console.log(`first request: ${first.body.playerId} ${first.body.nickname}`);
    // --8<-- [end:http-first-request]

    // --8<-- [start:http-request-shaping]
    const status = await client
      .get('/ops/nodes/game-server-1/status')
      .header('x-trace-id', 'tutorial-1')
      .timeout(5000)
      .async<{ processId: number }>();
    // The admin URL is different, so this one-shot request uses a separate client.
    const weight = await ZLinkHttpClient.create(adminBaseUrl)
      .basicAuth('ops', 'tutorial-admin')
      .post('/admin/channels/profile/weight')
      .query('value', '2')
      .async<WeightResult>();
    console.log(`request shaping: status ${status.status} weight ${weight.body.weight}`);
    // --8<-- [end:http-request-shaping]

    // --8<-- [start:http-json-body]
    const player = await client
      .post('/players/p2')
      .body({ nickname: 'rookie' })
      .async<string>();
    const room = await client
      .post('/rooms')
      .body({ title: 'tutorial-room' })
      .fetch<string>();
    // The room id returned here is used by the following room requests.
    const chat = await client
      .post(`/rooms/${room}/chat`)
      .body({ playerId: 'p2', text: 'hello' })
      .async<null>();
    console.log(`json body: player ${player.body} room ${room} chat ${chat.status}`);
    // --8<-- [end:http-json-body]

    // --8<-- [start:http-response-kinds]
    const typed = await client.get('/players/p2').async<PlayerInfo>();
    const raw = await client.get('/players/p2').submitRaw();
    const fetched = await client.get('/players/p2').fetch<PlayerInfo>();
    console.log(`response kinds: typed ${typed.status} raw ${raw.headers['content-type']} fetch ${fetched.nickname}`);
    // --8<-- [end:http-response-kinds]

    // --8<-- [start:http-compressed-response]
    const compressed = await ZLinkHttpClient.create(clientBaseUrl)
      .compression()
      .get(`/rooms/${room}`)
      .submitRaw();
    console.log(`compressed response: ${compressed.status} encoding-removed ${compressed.headers['content-encoding'] === undefined}`);
    // --8<-- [end:http-compressed-response]

    await client
      .post('/players/p1')
      .body({ nickname: 'rookie' })
      .submitRaw();

    // --8<-- [start:http-redirect]
    const redirected = await ZLinkHttpClient.create(clientBaseUrl)
      .followRedirects()
      .get('/player/p1')
      .async<PlayerInfo>();
    console.log(`redirect: ${redirected.status} ${redirected.body.playerId}`);
    // --8<-- [end:http-redirect]

    // --8<-- [start:http-basic-auth]
    const without = await ZLinkHttpClient.create(adminBaseUrl)
      .post('/admin/channels/profile/weight')
      .query('value', '2')
      .submitRaw();
    const withAuth = await ZLinkHttpClient.create(adminBaseUrl)
      .basicAuth('ops', 'tutorial-admin')
      .post('/admin/channels/profile/weight')
      .query('value', '2')
      .submitRaw();
    console.log(`basic auth: without ${without.status} with ${withAuth.status}`);
    // --8<-- [end:http-basic-auth]

    // --8<-- [start:http-download-stream]
    let downloadChunks = 0;
    let downloadBytes = 0;
    await client
      .get(`/rooms/${room}/export`)
      .download((chunk) => {
        downloadChunks++;
        downloadBytes += chunk.byteLength;
      });
    console.log(`download stream: chunks ${downloadChunks} bytes ${downloadBytes}`);
    // --8<-- [end:http-download-stream]

    // --8<-- [start:http-upload-stream]
    const uploadChunks = [
      encoder.encode('{"playerId":"p2","text":"one"}\n'),
      encoder.encode('{"playerId":"p2","text":"two"}\n'),
      encoder.encode('{"playerId":"p2","text":"three"}\n')
    ];
    let uploadIndex = 0;
    const imported = await client
      .post(`/rooms/${room}/import`)
      .bodyStream(() => uploadChunks[uploadIndex++] ?? null, 'application/x-ndjson')
      .fetch<ImportedResult>();
    console.log(`upload stream: imported ${imported.imported}`);
    // --8<-- [end:http-upload-stream]

    // --8<-- [start:http-error-kinds]
    let badRequestKind = 'unknown';
    try {
      await client.post('/players/p3').async<{ status: string }>();
    } catch (error: unknown) {
      badRequestKind = frameworkKindName(error);
    }
    let connectionRefusedKind = 'unknown';
    try {
      await ZLinkHttpClient.create('http://127.0.0.1:6380')
        .get('/players/p1')
        .submitRaw();
    } catch (error: unknown) {
      connectionRefusedKind = frameworkKindName(error);
    }
    console.log(`error kinds: bad request ${badRequestKind} connection refused ${connectionRefusedKind}`);
    // --8<-- [end:http-error-kinds]
  } finally {
    await client.close();
  }
}

function frameworkKindName(error: unknown): string {
  const kind = (error as { readonly kind?: unknown }).kind;
  if (typeof kind !== 'number') return String(kind);
  return [
    'NotFound',
    'AlreadyExists',
    'TypeMismatch',
    'NotConfigured',
    'Rejected',
    'Unavailable',
    'DeadlineExceeded',
    'ShuttingDown',
    'ProtocolError',
    'InvalidOperation',
    'DataLost',
    'InternalFailure'
  ][kind] ?? String(kind);
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
