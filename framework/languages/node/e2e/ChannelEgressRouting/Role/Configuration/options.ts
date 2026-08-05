export interface RoleOptions {
  readonly role: string;
  readonly rid: string;
  readonly instanceMarker: string;
  readonly httpUrl: string;
  readonly logDir: string;
  readonly evidenceFile?: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly gameEndpoint: string;
  readonly auditEndpoint: string;
  readonly workflowPort: number;
  readonly workflowWeight: number;
  readonly gamePeers: readonly string[];
  readonly auditPeers: readonly string[];
  readonly workflowPeers: readonly string[];
  readonly gameServers: readonly string[];
  readonly gameClients: readonly string[];
  readonly auditServers: readonly string[];
  readonly auditClients: readonly string[];
  readonly workflowClient: boolean;
  readonly workflowServer: boolean;
  readonly invalidMode?: string;
  readonly delayMs: number;
}

export function validateRoleOptions(value: unknown): RoleOptions {
  const values = objectValue(value);
  return {
    role: required(values, 'role'),
    rid: required(values, 'rid'),
    instanceMarker: required(values, 'instanceMarker'),
    httpUrl: required(values, 'httpUrl'),
    logDir: required(values, 'logDir'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    redisEndpoint: required(values, 'redisEndpoint'),
    redisKeyPrefix: required(values, 'redisKeyPrefix'),
    gameEndpoint: required(values, 'gameEndpoint'),
    auditEndpoint: required(values, 'auditEndpoint'),
    workflowPort: integer(values, 'workflowPort', 0),
    workflowWeight: integer(values, 'workflowWeight', 100),
    gamePeers: list(values, 'gamePeers'),
    auditPeers: list(values, 'auditPeers'),
    workflowPeers: list(values, 'workflowPeers'),
    gameServers: list(values, 'gameServers'),
    gameClients: list(values, 'gameClients'),
    auditServers: list(values, 'auditServers'),
    auditClients: list(values, 'auditClients'),
    workflowClient: boolean(values, 'workflowClient'),
    workflowServer: boolean(values, 'workflowServer'),
    invalidMode: optionalString(values, 'invalidMode'),
    delayMs: integer(values, 'delayMs', 0)
  };
}

function objectValue(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('Configuration section e2e must be an object.');
  }
  return value as Record<string, unknown>;
}

function optionalString(values: Record<string, unknown>, key: string): string | undefined {
  const value = values[key];
  return value === undefined ? undefined : typeof value === 'string' && value.length > 0
    ? value
    : (() => { throw new Error(`e2e.${key} must be a non-empty string.`); })();
}

function required(values: Record<string, unknown>, key: string): string {
  const value = values[key];
  if (typeof value !== 'string' || value.length === 0) throw new Error(`e2e.${key} is required.`);
  return value;
}

function integer(values: Record<string, unknown>, key: string, fallback: number): number {
  const value = values[key] === undefined ? fallback : Number(values[key]);
  if (!Number.isInteger(value) || value < 0) throw new Error(`e2e.${key} must be a non-negative integer.`);
  return value;
}

function boolean(values: Record<string, unknown>, key: string): boolean {
  const value = values[key];
  return value === true || value === 'true';
}

function list(values: Record<string, unknown>, key: string): readonly string[] {
  const value = values[key];
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== 'string' || entry.length === 0)) {
    throw new Error(`e2e.${key} must be an array of non-empty strings.`);
  }
  return value as string[];
}
