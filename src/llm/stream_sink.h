#ifndef LLM_STREAM_SINK_H
#define LLM_STREAM_SINK_H

/* Private to the adapters: turns a provider's SSE stream into StreamEvents
 * and reassembles the provider's final (non-streaming) response JSON, so the
 * adapters parse streamed and blocking responses with the same code. */

#include "llm/client.h"
#include "util/http.h"

typedef enum { SINK_ANTHROPIC, SINK_OPENAI, SINK_GEMINI } SinkKind;
typedef struct StreamSink StreamSink;

StreamSink   *stream_sink_new(SinkKind kind, StreamCallback cb, void *userdata);
void          stream_sink_free(StreamSink *sink);

/* Performs req as SSE. Returns a synthetic 200 response whose body is the
 * final provider JSON, the provider's error response for non-2xx statuses,
 * or NULL on transport failure, caller stop, or a malformed stream. */
HttpResponse *stream_sink_transport(StreamSink *sink, const HttpRequest *req);
bool          stream_sink_stopped(const StreamSink *sink);  /* callback returned false */
const char   *stream_sink_error(const StreamSink *sink);    /* stream-level failure, or NULL */

/* Terminal events: FINISH borrows response; ERROR carries message. */
void          stream_sink_finish(StreamSink *sink, LlmResponse *response);
void          stream_sink_fail(StreamSink *sink, const char *message);

#endif
