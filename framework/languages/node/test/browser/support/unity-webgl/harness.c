/*
 * Stands in for the IL2CPP-compiled side of com.zlink.stream-connector.webgl.
 *
 * Unity compiles the adapter's C# to C with IL2CPP and links the result with
 * emscripten. What reaches the jslib is therefore plain C: `extern` declarations
 * that match the `[DllImport("__Internal")]` signatures in
 * Runtime/Interop/ZlinkStreamInterop.cs, and a static function whose address is
 * handed over as the event sink - which is what `[AOT.MonoPInvokeCallback]`
 * produces. This file is that C, written by hand so the boundary can be linked
 * and run without Unity.
 *
 * Everything above the boundary mirrors Runtime/ZlinkStreamWebGlConnector.cs:
 * the sink only copies, the copies are transferred outside the sink, handlers
 * run only from the dispatch queue, and the wait surface consumes the unread
 * history without running a handler.
 */
#include <emscripten.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* The boundary, declared exactly as ZlinkStreamInterop.cs declares it. */
/* ------------------------------------------------------------------ */

extern int ZlinkStreamCreate(const char *optionsJson);
extern void ZlinkStreamDestroy(int handle);
extern char *ZlinkStreamTakeLastError(void);
extern void ZlinkStreamFreeBuffer(char *buffer);
extern int ZlinkStreamSetEventSink(int handle, void *callback);
extern int ZlinkStreamPump(int handle, int maxEvents);
extern void ZlinkStreamConnect(int handle, int callId);
extern void ZlinkStreamClose(int handle, int callId);
extern void ZlinkStreamDispatch(int handle, int callId);
extern void ZlinkStreamCancel(int handle, int callId);
extern void ZlinkStreamSend(int handle, int callId, const char *callJson,
                            const unsigned char *payload, int payloadLength);
extern void ZlinkStreamRequest(int handle, int callId, const char *callJson,
                               const unsigned char *payload, int payloadLength);
extern int ZlinkStreamObserve(int handle, const char *name);
extern void ZlinkStreamUnobserve(int handle, const char *name);
extern int ZlinkStreamIsConnected(int handle);
extern int ZlinkStreamGetState(int handle);
extern int ZlinkStreamGetCloseReason(int handle);
extern int ZlinkStreamGetPendingDispatchCount(int handle);
extern int ZlinkStreamGetDiagnosticsLevel(int handle);
extern int ZlinkStreamSetDiagnosticsLevel(int handle, int level);

#define EVENT_CALL_COMPLETED 1
#define EVENT_MESSAGE 2
#define EVENT_ERROR_RECEIVED 3
#define EVENT_DISCONNECTED 4
#define EVENT_STATE_CHANGED 5

#define MAX_EVENTS_PER_PUMP 256
#define CALL_SLOTS 8192
#define MAX_OBSERVERS 16
#define NAME_MAX_LEN 96
#define RETURN_BUFFER 65536

/* ------------------------------------------------------------------ */
/* Allocation bookkeeping for this side, so a leak measured on the heap */
/* can be attributed to one side of the boundary or the other.          */
/* ------------------------------------------------------------------ */

static int live_allocs = 0;

static void *hmalloc(size_t size) {
  void *block = malloc(size);
  if (block) live_allocs += 1;
  return block;
}

static void hfree(void *block) {
  if (!block) return;
  live_allocs -= 1;
  free(block);
}

static char *hstrdup(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = (char *)hmalloc(size);
  if (copy) memcpy(copy, text, size);
  return copy;
}

/* ------------------------------------------------------------------ */
/* Managed-side state                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  int id;      /* 0 when the slot is free */
  int state;   /* 0 pending, 1 completed, 2 failed */
  char *text;  /* reply descriptor or error JSON */
  unsigned char *bytes;
  int bytesLength;
} Call;

typedef struct Msg {
  int observerId;
  int codec;
  char *descriptor; /* {"name":...,"metadata":...} */
  unsigned char *bytes;
  int bytesLength;
  struct Msg *next;
} Msg;

typedef struct Item {
  int kind; /* EVENT_MESSAGE, EVENT_ERROR_RECEIVED, EVENT_DISCONNECTED, EVENT_STATE_CHANGED */
  Msg *message;
  char *text;
  struct Item *next;
} Item;

typedef struct {
  int id;
  char name[NAME_MAX_LEN];
  int hasHandler;
  Msg *unreadHead;
  Msg *unreadTail;
} Observer;

typedef struct Event {
  int type;
  int id;
  int value;
  char *text;
  unsigned char *bytes;
  int bytesLength;
  struct Event *next;
} Event;

static int connector = 0;
static int nextCallId = 1;
static Call calls[CALL_SLOTS];
static Observer observers[MAX_OBSERVERS];
static int observerCount = 0;

static Event *inboxHead = NULL;
static Event *inboxTail = NULL;
static Item *queueHead = NULL;
static Item *queueTail = NULL;

static int advanceCallId = 0;

static int sinkCalls = 0;
static int nestedPumpEnabled = 0;
static int nestedPumpCalls = 0;
static int nestedPumpNotRefused = 0;
static int handlerRuns = 0;
static int disconnectCount = 0;
static int errorCount = 0;
static int stateChangeCount = 0;

static char violations[1024];
static char handlerLog[16384];
static char stateLog[8192];
static char returnBuffer[RETURN_BUFFER];

static void note(const char *what) {
  if (strlen(violations) + strlen(what) + 2 >= sizeof(violations)) return;
  if (violations[0] != '\0') strcat(violations, "; ");
  strcat(violations, what);
}

static void appendTo(char *log, size_t capacity, const char *text) {
  size_t used = strlen(log);
  size_t extra = strlen(text);
  if (used + extra + 2 >= capacity) return;
  if (used > 0) log[used] = '\n';
  strcpy(log + (used > 0 ? used + 1 : 0), text);
}

static Call *slot(int callId) { return &calls[callId % CALL_SLOTS]; }

static void releaseCall(Call *call) {
  hfree(call->text);
  hfree(call->bytes);
  call->text = NULL;
  call->bytes = NULL;
  call->bytesLength = 0;
  call->id = 0;
  call->state = 0;
}

static int registerCall(void) {
  int callId = nextCallId++;
  Call *call = slot(callId);
  if (call->id != 0) releaseCall(call);
  call->id = callId;
  call->state = 0;
  return callId;
}

static Observer *observerById(int id) {
  for (int index = 0; index < observerCount; index += 1) {
    if (observers[index].id == id) return &observers[index];
  }
  return NULL;
}

static Observer *observerByName(const char *name) {
  for (int index = 0; index < observerCount; index += 1) {
    if (strcmp(observers[index].name, name) == 0) return &observers[index];
  }
  return NULL;
}

static void freeMsg(Msg *message) {
  hfree(message->descriptor);
  hfree(message->bytes);
  hfree(message);
}

static void enqueueItem(Item *item) {
  item->next = NULL;
  if (queueTail) {
    queueTail->next = item;
    queueTail = item;
  } else {
    queueHead = item;
    queueTail = item;
  }
}

/* ------------------------------------------------------------------ */
/* The event sink                                                       */
/*                                                                      */
/* This is the function whose address goes to ZlinkStreamSetEventSink.  */
/* IL2CPP emits exactly this shape for a static C# method carrying      */
/* [AOT.MonoPInvokeCallback(typeof(ZlinkStreamInterop.EventCallback))]. */
/* It copies and returns: the pointers belong to JavaScript and are     */
/* freed the moment it returns.                                         */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
void zlh_event_sink(int handle, int eventType, int id, int value, const char *text,
                    const unsigned char *bytes, int bytesLength) {
  sinkCalls += 1;

  if (handle != connector) note("sink received a foreign handle");
  if (!bytes && bytesLength != 0) note("null payload pointer with a non-zero length");
  if (bytes && bytesLength <= 0) note("payload pointer with a non-positive length");

  if (nestedPumpEnabled) {
    /* Worst case: managed code re-enters the boundary from inside the callback. */
    int nested = ZlinkStreamPump(handle, MAX_EVENTS_PER_PUMP);
    nestedPumpCalls += 1;
    if (nested != -1) nestedPumpNotRefused += 1;
  }

  Event *event = (Event *)hmalloc(sizeof(Event));
  event->type = eventType;
  event->id = id;
  event->value = value;
  event->text = text ? hstrdup(text) : NULL;
  event->bytesLength = bytesLength;
  event->bytes = NULL;
  if (bytes && bytesLength > 0) {
    event->bytes = (unsigned char *)hmalloc((size_t)bytesLength);
    memcpy(event->bytes, bytes, (size_t)bytesLength);
  }
  event->next = NULL;

  if (inboxTail) {
    inboxTail->next = event;
    inboxTail = event;
  } else {
    inboxHead = event;
    inboxTail = event;
  }
}

/* ------------------------------------------------------------------ */
/* Transfer: runs outside the sink, like the managed connector's pump.  */
/* ------------------------------------------------------------------ */

static const char *findMember(const char *json, const char *key) {
  return json ? strstr(json, key) : NULL;
}

/* Reads "name":"<value>" out of the descriptor without pulling in a JSON parser. */
static void readStringMember(const char *json, const char *key, char *out, size_t capacity) {
  out[0] = '\0';
  const char *found = findMember(json, key);
  if (!found) return;
  const char *cursor = found + strlen(key);
  while (*cursor == ' ' || *cursor == ':') cursor += 1;
  if (*cursor != '"') return;
  cursor += 1;
  size_t length = 0;
  while (*cursor && *cursor != '"' && length + 1 < capacity) {
    out[length++] = *cursor++;
  }
  out[length] = '\0';
}

static void transferEvent(Event *event) {
  switch (event->type) {
    case EVENT_CALL_COMPLETED: {
      Call *call = slot(event->id);
      if (call->id != event->id) return;
      call->state = event->value == 1 ? 1 : 2;
      call->text = event->text;
      event->text = NULL;
      call->bytes = event->bytes;
      call->bytesLength = event->bytesLength;
      event->bytes = NULL;
      return;
    }

    case EVENT_MESSAGE: {
      Observer *observer = observerById(event->id);
      if (!observer) return;
      Msg *message = (Msg *)hmalloc(sizeof(Msg));
      message->observerId = event->id;
      message->codec = event->value;
      message->descriptor = event->text;
      event->text = NULL;
      message->bytes = event->bytes;
      message->bytesLength = event->bytesLength;
      event->bytes = NULL;
      message->next = NULL;

      if (observer->hasHandler) {
        Item *item = (Item *)hmalloc(sizeof(Item));
        item->kind = EVENT_MESSAGE;
        item->message = message;
        item->text = NULL;
        enqueueItem(item);
        return;
      }

      if (observer->unreadTail) {
        observer->unreadTail->next = message;
        observer->unreadTail = message;
      } else {
        observer->unreadHead = message;
        observer->unreadTail = message;
      }
      return;
    }

    default: {
      Item *item = (Item *)hmalloc(sizeof(Item));
      item->kind = event->type;
      item->message = NULL;
      item->text = event->text;
      event->text = NULL;
      enqueueItem(item);
      return;
    }
  }
}

static void drainInbox(void) {
  while (inboxHead) {
    Event *event = inboxHead;
    inboxHead = event->next;
    if (!inboxHead) inboxTail = NULL;
    transferEvent(event);
    hfree(event->text);
    hfree(event->bytes);
    hfree(event);
  }
}

/* ------------------------------------------------------------------ */
/* Exported surface for the page driver                                 */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
const char *zlh_take_last_error(void) {
  char *pointer = ZlinkStreamTakeLastError();
  if (!pointer) {
    returnBuffer[0] = '\0';
    return returnBuffer;
  }
  snprintf(returnBuffer, sizeof(returnBuffer), "%s", pointer);
  /* Ownership of this one buffer is the managed side's. */
  ZlinkStreamFreeBuffer(pointer);
  return returnBuffer;
}

/* Tears the managed side down between scenarios: destroys the connector, frees
   every copy still held, and clears the counters and logs. */
EMSCRIPTEN_KEEPALIVE
void zlh_reset(void) {
  if (connector) {
    ZlinkStreamDestroy(connector);
    connector = 0;
  }
  advanceCallId = 0;

  while (inboxHead) {
    Event *event = inboxHead;
    inboxHead = event->next;
    hfree(event->text);
    hfree(event->bytes);
    hfree(event);
  }
  inboxTail = NULL;

  while (queueHead) {
    Item *item = queueHead;
    queueHead = item->next;
    if (item->message) freeMsg(item->message);
    hfree(item->text);
    hfree(item);
  }
  queueTail = NULL;

  for (int index = 0; index < observerCount; index += 1) {
    Msg *message = observers[index].unreadHead;
    while (message) {
      Msg *next = message->next;
      freeMsg(message);
      message = next;
    }
    observers[index].unreadHead = NULL;
    observers[index].unreadTail = NULL;
  }
  observerCount = 0;

  for (int index = 0; index < CALL_SLOTS; index += 1) {
    if (calls[index].id != 0) releaseCall(&calls[index]);
  }

  sinkCalls = 0;
  nestedPumpEnabled = 0;
  nestedPumpCalls = 0;
  nestedPumpNotRefused = 0;
  handlerRuns = 0;
  disconnectCount = 0;
  errorCount = 0;
  stateChangeCount = 0;
  violations[0] = '\0';
  handlerLog[0] = '\0';
  stateLog[0] = '\0';
}

EMSCRIPTEN_KEEPALIVE
int zlh_create(const char *optionsJson) {
  connector = ZlinkStreamCreate(optionsJson);
  if (connector == 0) return 0;
  if (ZlinkStreamSetEventSink(connector, (void *)&zlh_event_sink) != 1) {
    note("ZlinkStreamSetEventSink refused the function pointer");
    return 0;
  }
  return connector;
}

EMSCRIPTEN_KEEPALIVE
void zlh_destroy(void) {
  if (!connector) return;
  ZlinkStreamDestroy(connector);
  connector = 0;
}

EMSCRIPTEN_KEEPALIVE
int zlh_pump(void) {
  if (!connector) return 0;
  int drained = ZlinkStreamPump(connector, MAX_EVENTS_PER_PUMP);
  drainInbox();
  return drained;
}

/*
 * One Unity Update(): keep a Dispatch call in flight so the transport advances,
 * then pump the boundary and transfer what came back. Handlers do not run here;
 * zlh_run_dispatch_queue is the explicit Dispatch step.
 */
EMSCRIPTEN_KEEPALIVE
int zlh_frame(void) {
  if (!connector) return 0;
  /* Dispatch only advances a live transport, so a frame before Connect or
     after Close only pumps - the same rule the managed connector follows. */
  if (ZlinkStreamIsConnected(connector) == 1 &&
      (advanceCallId == 0 || slot(advanceCallId)->id != advanceCallId ||
       slot(advanceCallId)->state != 0)) {
    advanceCallId = registerCall();
    ZlinkStreamDispatch(connector, advanceCallId);
  }
  return zlh_pump();
}

/*
 * Whether the Dispatch that zlh_frame keeps in flight has settled. The connector
 * allows one pending stream read at a time, so an explicit Dispatch step waits
 * for this one rather than starting a second.
 */
EMSCRIPTEN_KEEPALIVE
int zlh_advance_idle(void) {
  if (advanceCallId == 0) return 1;
  Call *call = slot(advanceCallId);
  if (call->id != advanceCallId) return 1;
  return call->state == 0 ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE
int zlh_connect(void) {
  int callId = registerCall();
  ZlinkStreamConnect(connector, callId);
  return callId;
}

EMSCRIPTEN_KEEPALIVE
int zlh_close(void) {
  int callId = registerCall();
  ZlinkStreamClose(connector, callId);
  return callId;
}

EMSCRIPTEN_KEEPALIVE
int zlh_dispatch_call(void) {
  int callId = registerCall();
  ZlinkStreamDispatch(connector, callId);
  return callId;
}

/*
 * Send and Request own the payload buffer. The jslib copies the bytes before it
 * returns, so the buffer is poisoned and freed the moment the call is back -
 * if the JavaScript side ever kept the pointer, the wire would carry 0xFF.
 */
static int submit(int request, const char *callJson, const char *payload) {
  int callId = registerCall();
  int length = (int)strlen(payload);
  unsigned char *buffer = (unsigned char *)hmalloc((size_t)(length > 0 ? length : 1));
  memcpy(buffer, payload, (size_t)length);
  if (request) {
    ZlinkStreamRequest(connector, callId, callJson, buffer, length);
  } else {
    ZlinkStreamSend(connector, callId, callJson, buffer, length);
  }
  memset(buffer, 0xFF, (size_t)(length > 0 ? length : 1));
  hfree(buffer);
  return callId;
}

EMSCRIPTEN_KEEPALIVE
int zlh_send(const char *callJson, const char *payload) { return submit(0, callJson, payload); }

EMSCRIPTEN_KEEPALIVE
int zlh_request(const char *callJson, const char *payload) { return submit(1, callJson, payload); }

EMSCRIPTEN_KEEPALIVE
int zlh_cancel(int callId) {
  ZlinkStreamCancel(connector, callId);
  return callId;
}

EMSCRIPTEN_KEEPALIVE
int zlh_call_state(int callId) {
  Call *call = slot(callId);
  if (call->id != callId) return -1;
  return call->state;
}

EMSCRIPTEN_KEEPALIVE
const char *zlh_call_text(int callId) {
  Call *call = slot(callId);
  returnBuffer[0] = '\0';
  if (call->id == callId && call->text) snprintf(returnBuffer, sizeof(returnBuffer), "%s", call->text);
  return returnBuffer;
}

EMSCRIPTEN_KEEPALIVE
const char *zlh_call_payload(int callId) {
  Call *call = slot(callId);
  returnBuffer[0] = '\0';
  if (call->id == callId && call->bytes && call->bytesLength > 0) {
    int length = call->bytesLength;
    if (length > (int)sizeof(returnBuffer) - 1) length = (int)sizeof(returnBuffer) - 1;
    memcpy(returnBuffer, call->bytes, (size_t)length);
    returnBuffer[length] = '\0';
  }
  return returnBuffer;
}

EMSCRIPTEN_KEEPALIVE
void zlh_release_call(int callId) {
  Call *call = slot(callId);
  if (call->id == callId) releaseCall(call);
}

EMSCRIPTEN_KEEPALIVE
int zlh_observe(const char *name) {
  Observer *existing = observerByName(name);
  if (existing) return existing->id;
  if (observerCount >= MAX_OBSERVERS) return 0;
  int id = ZlinkStreamObserve(connector, name);
  Observer *observer = &observers[observerCount++];
  observer->id = id;
  snprintf(observer->name, sizeof(observer->name), "%s", name);
  observer->hasHandler = 0;
  observer->unreadHead = NULL;
  observer->unreadTail = NULL;
  return id;
}

EMSCRIPTEN_KEEPALIVE
void zlh_register_handler(const char *name) {
  zlh_observe(name);
  Observer *observer = observerByName(name);
  if (observer) observer->hasHandler = 1;
}

EMSCRIPTEN_KEEPALIVE
int zlh_pending_dispatch(void) {
  int count = 0;
  for (Item *item = queueHead; item; item = item->next) count += 1;
  return count;
}

EMSCRIPTEN_KEEPALIVE
int zlh_jslib_pending_dispatch(void) { return ZlinkStreamGetPendingDispatchCount(connector); }

/* The explicit Dispatch step: this is the only place a handler ever runs. */
EMSCRIPTEN_KEEPALIVE
int zlh_run_dispatch_queue(void) {
  int ran = 0;
  while (queueHead) {
    Item *item = queueHead;
    queueHead = item->next;
    if (!queueHead) queueTail = NULL;

    if (item->kind == EVENT_MESSAGE) {
      char name[NAME_MAX_LEN];
      readStringMember(item->message->descriptor, "\"name\"", name, sizeof(name));
      char line[512];
      int length = item->message->bytesLength;
      if (length > 400) length = 400;
      snprintf(line, sizeof(line), "%s|%d|%.*s", name, item->message->codec, length,
               (const char *)item->message->bytes);
      appendTo(handlerLog, sizeof(handlerLog), line);
      handlerRuns += 1;
      freeMsg(item->message);
    } else if (item->kind == EVENT_STATE_CHANGED) {
      stateChangeCount += 1;
      appendTo(stateLog, sizeof(stateLog), item->text ? item->text : "null");
      hfree(item->text);
    } else if (item->kind == EVENT_DISCONNECTED) {
      disconnectCount += 1;
      hfree(item->text);
    } else {
      errorCount += 1;
      hfree(item->text);
    }

    hfree(item);
    ran += 1;
  }
  return ran;
}

/*
 * The wait surface: consumes the unread history without running a handler,
 * which is the WaitFor/ExpectNone/WaitForSequence rule in spec 32 section 10.1.1.
 */
EMSCRIPTEN_KEEPALIVE
const char *zlh_take_unread(const char *name) {
  returnBuffer[0] = '\0';
  Observer *observer = observerByName(name);
  if (!observer || !observer->unreadHead) return returnBuffer;
  Msg *message = observer->unreadHead;
  observer->unreadHead = message->next;
  if (!observer->unreadHead) observer->unreadTail = NULL;

  char descriptorName[NAME_MAX_LEN];
  readStringMember(message->descriptor, "\"name\"", descriptorName, sizeof(descriptorName));
  int length = message->bytesLength;
  if (length > (int)sizeof(returnBuffer) - 256) length = (int)sizeof(returnBuffer) - 256;
  snprintf(returnBuffer, sizeof(returnBuffer), "%s|%d|%.*s", descriptorName, message->codec, length,
           (const char *)message->bytes);
  freeMsg(message);
  return returnBuffer;
}

EMSCRIPTEN_KEEPALIVE
int zlh_received_count(const char *name) {
  Observer *observer = observerByName(name);
  if (!observer) return 0;
  int count = 0;
  for (Msg *message = observer->unreadHead; message; message = message->next) count += 1;
  return count;
}

EMSCRIPTEN_KEEPALIVE
int zlh_is_connected(void) { return ZlinkStreamIsConnected(connector); }

EMSCRIPTEN_KEEPALIVE
int zlh_state(void) { return ZlinkStreamGetState(connector); }

EMSCRIPTEN_KEEPALIVE
int zlh_close_reason(void) { return ZlinkStreamGetCloseReason(connector); }

EMSCRIPTEN_KEEPALIVE
int zlh_diagnostics_level(void) { return ZlinkStreamGetDiagnosticsLevel(connector); }

EMSCRIPTEN_KEEPALIVE
int zlh_set_diagnostics_level(int level) { return ZlinkStreamSetDiagnosticsLevel(connector, level); }

EMSCRIPTEN_KEEPALIVE
void zlh_unobserve(const char *name) { ZlinkStreamUnobserve(connector, name); }

EMSCRIPTEN_KEEPALIVE
void zlh_set_nested_pump(int enabled) { nestedPumpEnabled = enabled; }

EMSCRIPTEN_KEEPALIVE
int zlh_nested_pump_calls(void) { return nestedPumpCalls; }

EMSCRIPTEN_KEEPALIVE
int zlh_nested_pump_not_refused(void) { return nestedPumpNotRefused; }

EMSCRIPTEN_KEEPALIVE
int zlh_sink_calls(void) { return sinkCalls; }

EMSCRIPTEN_KEEPALIVE
int zlh_handler_runs(void) { return handlerRuns; }

EMSCRIPTEN_KEEPALIVE
int zlh_disconnect_count(void) { return disconnectCount; }

EMSCRIPTEN_KEEPALIVE
int zlh_error_count(void) { return errorCount; }

EMSCRIPTEN_KEEPALIVE
int zlh_state_change_count(void) { return stateChangeCount; }

EMSCRIPTEN_KEEPALIVE
const char *zlh_handler_log(void) {
  snprintf(returnBuffer, sizeof(returnBuffer), "%s", handlerLog);
  return returnBuffer;
}

EMSCRIPTEN_KEEPALIVE
const char *zlh_state_log(void) {
  snprintf(returnBuffer, sizeof(returnBuffer), "%s", stateLog);
  return returnBuffer;
}

EMSCRIPTEN_KEEPALIVE
const char *zlh_violations(void) {
  snprintf(returnBuffer, sizeof(returnBuffer), "%s", violations);
  return returnBuffer;
}

/* dlmalloc's own accounting: bytes handed out and not yet returned. */
EMSCRIPTEN_KEEPALIVE
int zlh_heap_in_use(void) {
  struct mallinfo info = mallinfo();
  return (int)info.uordblks;
}

EMSCRIPTEN_KEEPALIVE
int zlh_heap_break(void) { return (int)(intptr_t)sbrk(0); }

/* Allocations this file made and has not freed, so a heap delta can be split. */
EMSCRIPTEN_KEEPALIVE
int zlh_live_allocs(void) { return live_allocs; }

int main(void) {
  violations[0] = '\0';
  handlerLog[0] = '\0';
  stateLog[0] = '\0';
  return 0;
}
