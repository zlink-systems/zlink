import * as fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

type SharedSettings = {
  redisEndpoint: string;
  redisKeyPrefix: string;
  logDirectory: string;
};

type ZoneNodeSettings = {
  nodeId: string;
  spotRouterEndpoint: string;
  faultTickZone?: string | null;
  faultTickSignalPath?: string;
  disableBots?: boolean;
  botStartSignalPath?: string;
  waitForPlacementPeer?: boolean;
  placementWeightAfterZoneCreation?: number;
};

type GatewaySettings = {
  streamEndpoint: string;
  spotRouterEndpoint: string;
};

type OpsSettings = {
  streamEndpoint: string;
  broadcastEndpoint: string;
  reportEndpoint: string;
};

type ClientSettings = {
  gatewayEndpoint: string;
  opsEndpoint: string;
  scenarios?: string;
};

type ZoneWorldConfiguration = {
  shared: SharedSettings;
  zoneNode?: ZoneNodeSettings;
  gateway?: GatewaySettings;
  ops?: OpsSettings;
  client?: ClientSettings;
};

const ZONEWORLD_CONFIG = Symbol.for('ZONEWORLD_CONFIG');
class ZoneWorldConfigurationModule {}
Module({})(ZoneWorldConfigurationModule);

function createZoneWorldConfigurationModule(role: keyof Omit<ZoneWorldConfiguration, 'shared'>): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  return {
    module: ZoneWorldConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ zoneworld: readConfiguration(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: ZONEWORLD_CONFIG,
      inject: [ConfigService],
      useFactory: (service: ConfigService) => validateConfiguration(service.get('zoneworld'), role)
    }],
    exports: [ZONEWORLD_CONFIG]
  };
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readConfiguration(configPath: string): unknown {
  return JSON.parse(fs.readFileSync(configPath, 'utf8')) as unknown;
}

function validateConfiguration(
  value: unknown,
  expectedRole: keyof Omit<ZoneWorldConfiguration, 'shared'>
): ZoneWorldConfiguration {
  const root = requireRecord(value, 'ZoneWorld configuration');
  const document = requireRecord(root.sample, 'sample');
  const shared = requireRecord(document.shared, 'shared');
  for (const key of ['redisEndpoint', 'redisKeyPrefix', 'logDirectory']) requireString(shared, key, 'shared');

  const roles = ['zoneNode', 'gateway', 'ops', 'client'] as const;
  const configured = roles.filter((role) => document[role] !== undefined);
  if (configured.length !== 1 || configured[0] !== expectedRole) {
    throw new Error(`Exactly the '${expectedRole}' ZoneWorld role must be configured.`);
  }
  const role = requireRecord(document[expectedRole], expectedRole);
  for (const key of roleKeys(expectedRole)) requireString(role, key, expectedRole);
  if (expectedRole === 'zoneNode' && role.botStartSignalPath !== undefined) {
    requireString(role, 'botStartSignalPath', expectedRole);
  }
  if (expectedRole === 'zoneNode' && role.faultTickSignalPath !== undefined) {
    requireString(role, 'faultTickSignalPath', expectedRole);
  }
  if (expectedRole === 'zoneNode' && role.placementWeightAfterZoneCreation !== undefined) {
    const weight = role.placementWeightAfterZoneCreation;
    if (typeof weight !== 'number' || !Number.isSafeInteger(weight) || weight < 0 || weight > 10_000) {
      throw new Error(
        `Configuration value '${expectedRole}.placementWeightAfterZoneCreation' must be an integer from 0 through 10000.`
      );
    }
  }
  return document as ZoneWorldConfiguration;
}

function roleKeys(role: keyof Omit<ZoneWorldConfiguration, 'shared'>): readonly string[] {
  switch (role) {
    case 'zoneNode': return [
      'nodeId', 'spotRouterEndpoint'
    ];
    case 'gateway': return ['streamEndpoint', 'spotRouterEndpoint'];
    case 'ops': return ['streamEndpoint', 'broadcastEndpoint', 'reportEndpoint'];
    case 'client': return ['gatewayEndpoint', 'opsEndpoint'];
  }
}

function requireRecord(value: unknown, label: string): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`Configuration section '${label}' must be an object.`);
  }
  return value as Record<string, unknown>;
}

function requireString(record: Record<string, unknown>, key: string, section: string): void {
  if (typeof record[key] !== 'string' || (record[key] as string).trim().length === 0) {
    throw new Error(`Configuration value '${section}.${key}' must be a non-empty string.`);
  }
}

export { ZONEWORLD_CONFIG, createZoneWorldConfigurationModule, readConfigPath, validateConfiguration };
export type {
  ClientSettings,
  GatewaySettings,
  OpsSettings,
  SharedSettings,
  ZoneNodeSettings,
  ZoneWorldConfiguration
};
