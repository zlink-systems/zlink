import { createRequire } from 'node:module';
import path from 'node:path';

const requireFramework = createRequire(__filename);
const frameworkEntry = requireFramework.resolve('@zlink-systems/framework');
const internal = requireFramework(
  path.join(path.dirname(frameworkEntry), 'internal')
) as typeof import('../../framework/src/internal');

export const {
  STORE_COUNTER_MAX,
  STORE_COUNTER_MAX_BYTES,
  validateStoreWriteRequest,
  ZLINK_PROVIDER_MAX_KEY_BYTES,
  ZLINK_PROVIDER_MAX_ENCODED_PAGE_BYTES,
  ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES,
  ZLINK_PROVIDER_MAX_PAGE_SIZE,
  ZLINK_RELOCATION_MAX_BLOB_REFERENCE_BYTES
} = internal;
