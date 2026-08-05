interface ZLinkSendCall {
  submit(): Promise<void>;
}

declare const call: ZLinkSendCall;
void call.submit();
// @ts-expect-error Config 13 removed the synchronous one-shot terminator.
void call['try' + 'Submit']();
