/* SPDX-License-Identifier: Apache-2.0 */

// Version identity for outgoing requests. The single source of truth is the package.json
// version; the User-Agent product token is derived as zlink-http-client/<major.minor>.
// The relative path works both for the source tree and for the packaged dist/ layout.
const packageVersion = (require('../../package.json') as { version: string }).version;

export const httpClientUserAgent = `zlink-http-client/${packageVersion
  .split('.')
  .slice(0, 2)
  .join('.')}`;
