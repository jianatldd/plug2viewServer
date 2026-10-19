/* Optional single-camera Google Home experiment. No cloud SDK or background process. */
#ifndef PLUG2VIEW_GOOGLE_H
#define PLUG2VIEW_GOOGLE_H
#include <stddef.h>
/* Google supplies opaque state. Reserve the full worst-case percent-encoded
 * redirect plus the callback URL and authorization code. */
#define GOOGLE_STATE_MAX 2048
#define GOOGLE_RESPONSE_EXTRA (3 * GOOGLE_STATE_MAX + 512)
struct google_response { int code; const char *type; char extra[GOOGLE_RESPONSE_EXTRA]; char body[12288]; };
int google_init(const char *config);
int google_enabled(void);
enum google_control_kind { GOOGLE_MOTION, GOOGLE_RECORDING, GOOGLE_PRESET };
/* value -1 queries; other values set a switch or activate preset 0..10.
 * Return NULL on success, otherwise a Google device error code. */
typedef const char *(*google_control_fn)(enum google_control_kind kind,int value,int *state);
void google_set_controls(google_control_fn callback);
int google_header(const char *headers, const char *name, char *out, size_t cap);
int google_param(const char *form, const char *key, char *out, size_t cap);
int google_stream_token(const char *query, char *token, size_t cap);
/* Called only after camd has authenticated the website viewer. */
void google_viewer_webrtc(const char *body, struct google_response *response);
void google_handle(const char *method, const char *path, const char *query,
                   const char *body, const char *headers, int secure, int online,
                   struct google_response *response);
#endif
