// MON-D1A: 등록하지 않은 MeshName 조회를 거부한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runMonD1A(options: ClientOptions): Promise<void> {
  const queryError = await expectConfigurationFailure(options.serviceUrl, '/status/route/missing');
  const observeError = await expectConfigurationFailure(options.serviceUrl, '/status/route/missing/observe');
  ensure(queryError.length > 0 && observeError.length > 0, 'MON-D1A missing MeshName error was empty.');
  const current = await getJson<{ readonly meshName: string }>(options.serviceUrl, '/status/route');
  ensure(current.meshName === 'monitor.profile', 'MON-D1A invalid query changed the registered RouteMesh status.');
  console.log('scenario MON-D1A passed');
}

async function expectConfigurationFailure(url: string, path: string): Promise<string> {
  try {
    await getJson(url, path);
  } catch (error) {
    return error instanceof Error ? error.message : String(error);
  }
  throw new Error(`MON-D1A expected '${path}' to reject an unregistered MeshName.`);
}
