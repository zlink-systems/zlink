import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { PacketNames, RegistrationCodecNames } from '../../Shared/messages';
import { validateInvalidDuplicateOptions, type InvalidDuplicateOptions } from './Configuration/invalid-duplicate-options';
import { REGISTRATION_CODEC_OPTIONS, createRegistrationCodecConfigurationModule } from '../../configuration';
import { DuplicateEchoRequestHandler } from './Handlers/duplicate-handlers';

export async function startInvalidDuplicate(): Promise<void> {
  const InvalidDuplicateModule = createInvalidDuplicateModule();
  const app = await NestFactory.createApplicationContext(InvalidDuplicateModule, { logger: false, abortOnError: false });
  await app.close();
}

function createInvalidDuplicateModule(): Function {
  class InvalidDuplicateModule {}
  const configuration = createRegistrationCodecConfigurationModule(validateInvalidDuplicateOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [REGISTRATION_CODEC_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as InvalidDuplicateOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow('normal');
          const mesh = builder.addRouteMesh(RegistrationCodecNames.channel)
            .listen(options.channelEndpoint);
          const channel = mesh.channel(RegistrationCodecNames.channel).server();
          if (options.invalidCase === 'missing-handler-group') {
            // A handler group is optional metadata. Exercise the required
            // handler validation with a real ClientServer role that has no
            // request or send handler.
            builder.addClientServerChannel('missing-handler').server().listen(0);
          } else {
            channel.addRequestHandler(PacketNames.echoManualReq, DuplicateEchoRequestHandler);
          }
          if (options.invalidCase === 'duplicate') {
            channel.addRequestHandler(PacketNames.echoManualReq, DuplicateEchoRequestHandler);
          }
          if (options.invalidCase === 'mixed-channel-kinds') {
            builder.addFanoutChannel(RegistrationCodecNames.channel)
              .enablePublisher(options.channelEndpoint);
          }
          return builder.build();
        }
      })
    ],
    providers: [DuplicateEchoRequestHandler]
  })(InvalidDuplicateModule);
  return InvalidDuplicateModule;
}
