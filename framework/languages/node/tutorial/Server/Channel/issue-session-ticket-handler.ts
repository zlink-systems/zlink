import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkRequestHandler } from '@zlink-systems/framework';
import type { IssueSessionTicket, SessionTicket } from '../../Shared/contracts';

// --8<-- [start:clientserver-handler]
// A ClientServer channel handler is written exactly like a RouteMesh one. Only
// the way the caller reaches it differs.
@Injectable()
export class IssueSessionTicketHandler implements ZLinkRequestHandler<
  IssueSessionTicket,
  SessionTicket
> {
  async handle(request: IssueSessionTicket, context: ZLinkMessageContext): Promise<SessionTicket> {
    void context;
    return { value: `ticket-${request.playerId}` };
  }
}
// --8<-- [end:clientserver-handler]
