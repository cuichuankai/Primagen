#include "feishu_ws.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include "../vendor/mongoose/mongoose.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
  char *key;
  char *value;
} WSHeader;

typedef struct {
  uint64_t seq_id;
  uint64_t log_id;
  int32_t service;
  int32_t method;
  WSHeader headers[64];
  size_t headers_count;
  char *payload_type;
  unsigned char *payload;
  size_t payload_len;
} WSFrame;

typedef struct {
  char *msg_id;
  int sum;
  char **parts;
  size_t *part_lens;
  uint64_t created_ms;
} PendingMessage;

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} ByteBuf;

struct FeishuWS {
  struct mg_mgr mgr;
  struct mg_connection *c;
  bool running;
  FeishuWSMessageCallback callback;
  void *user_data;
  char *recent_msg_ids[20];
  int msg_id_idx;
  int32_t service_id;
  uint64_t ping_interval_ms;
  uint64_t next_ping_ms;
  PendingMessage pending[16];
};

static uint64_t now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000 + (uint64_t) tv.tv_usec / 1000;
}

static uint64_t read_varint(const unsigned char *data, size_t len,
                            size_t *offset) {
  uint64_t value = 0;
  int shift = 0;
  while (*offset < len) {
    unsigned char b = data[*offset];
    (*offset)++;
    value |= ((uint64_t) (b & 0x7F)) << shift;
    if ((b & 0x80) == 0) break;
    shift += 7;
  }
  return value;
}

static void skip_field(const unsigned char *data, size_t len, size_t *offset,
                       int wire_type) {
  if (wire_type == 0) {
    read_varint(data, len, offset);
  } else if (wire_type == 2) {
    uint64_t l = read_varint(data, len, offset);
    if (*offset + l > len)
      *offset = len;
    else
      *offset += l;
  } else if (wire_type == 1) {
    *offset += 8;
  } else if (wire_type == 5) {
    *offset += 4;
  }
  if (*offset > len) *offset = len;
}

static bool bb_ensure(ByteBuf *b, size_t extra) {
  size_t need = b->len + extra;
  size_t ncap = b->cap ? b->cap : 256;
  unsigned char *p;
  if (need <= b->cap) return true;
  while (ncap < need) ncap *= 2;
  p = (unsigned char *) realloc(b->data, ncap);
  if (!p) return false;
  b->data = p;
  b->cap = ncap;
  return true;
}

static bool bb_add(ByteBuf *b, const void *p, size_t n) {
  if (!bb_ensure(b, n)) return false;
  memcpy(b->data + b->len, p, n);
  b->len += n;
  return true;
}

static bool bb_add_u8(ByteBuf *b, uint8_t v) { return bb_add(b, &v, 1); }

static bool bb_add_varint(ByteBuf *b, uint64_t v) {
  while (v >= 0x80) {
    uint8_t c = (uint8_t) ((v & 0x7f) | 0x80);
    if (!bb_add_u8(b, c)) return false;
    v >>= 7;
  }
  return bb_add_u8(b, (uint8_t) v);
}

static bool pb_add_key(ByteBuf *b, int field, int wt) {
  return bb_add_varint(b, ((uint64_t) field << 3) | (uint64_t) wt);
}

static bool pb_add_bytes(ByteBuf *b, int field, const void *p, size_t n) {
  if (!pb_add_key(b, field, 2)) return false;
  if (!bb_add_varint(b, n)) return false;
  return n == 0 ? true : bb_add(b, p, n);
}

static bool pb_add_varint_field(ByteBuf *b, int field, uint64_t v) {
  if (!pb_add_key(b, field, 0)) return false;
  return bb_add_varint(b, v);
}

static const char *frame_header_get(WSFrame *f, const char *k) {
  size_t i;
  for (i = 0; i < f->headers_count; i++) {
    if (f->headers[i].key && strcmp(f->headers[i].key, k) == 0)
      return f->headers[i].value;
  }
  return NULL;
}

static int frame_header_get_int(WSFrame *f, const char *k) {
  const char *v = frame_header_get(f, k);
  return v ? atoi(v) : 0;
}

static bool frame_header_set(WSFrame *f, const char *k, const char *v) {
  size_t i;
  for (i = 0; i < f->headers_count; i++) {
    if (f->headers[i].key && strcmp(f->headers[i].key, k) == 0) {
      char *nv = strdup(v ? v : "");
      if (!nv) return false;
      free(f->headers[i].value);
      f->headers[i].value = nv;
      return true;
    }
  }
  if (f->headers_count >= sizeof(f->headers) / sizeof(f->headers[0]))
    return false;
  f->headers[f->headers_count].key = strdup(k);
  f->headers[f->headers_count].value = strdup(v ? v : "");
  if (!f->headers[f->headers_count].key || !f->headers[f->headers_count].value)
    return false;
  f->headers_count++;
  return true;
}

static void frame_free(WSFrame *f) {
  size_t i;
  for (i = 0; i < f->headers_count; i++) {
    free(f->headers[i].key);
    free(f->headers[i].value);
  }
  free(f->payload_type);
  free(f->payload);
  memset(f, 0, sizeof(*f));
}

static bool parse_header_msg(const unsigned char *p, size_t n, WSHeader *h) {
  size_t off = 0;
  while (off < n) {
    uint64_t k = read_varint(p, n, &off);
    int f = (int) (k >> 3), wt = (int) (k & 7);
    if (wt == 2) {
      uint64_t l = read_varint(p, n, &off);
      if (off + l > n) return false;
      if (f == 1) {
        h->key = (char *) malloc((size_t) l + 1);
        if (!h->key) return false;
        memcpy(h->key, p + off, (size_t) l);
        h->key[l] = 0;
      } else if (f == 2) {
        h->value = (char *) malloc((size_t) l + 1);
        if (!h->value) return false;
        memcpy(h->value, p + off, (size_t) l);
        h->value[l] = 0;
      }
      off += (size_t) l;
    } else {
      skip_field(p, n, &off, wt);
    }
  }
  return true;
}

static bool parse_frame(const unsigned char *p, size_t n, WSFrame *f) {
  size_t off = 0;
  while (off < n) {
    uint64_t k = read_varint(p, n, &off);
    int fn = (int) (k >> 3), wt = (int) (k & 7);
    if (fn == 1 && wt == 0) {
      f->seq_id = read_varint(p, n, &off);
    } else if (fn == 2 && wt == 0) {
      f->log_id = read_varint(p, n, &off);
    } else if (fn == 3 && wt == 0) {
      f->service = (int32_t) read_varint(p, n, &off);
    } else if (fn == 4 && wt == 0) {
      f->method = (int32_t) read_varint(p, n, &off);
    } else if (fn == 5 && wt == 2) {
      uint64_t l = read_varint(p, n, &off);
      WSHeader h = {0};
      if (off + l > n) return false;
      if (f->headers_count >= sizeof(f->headers) / sizeof(f->headers[0]))
        return false;
      if (!parse_header_msg(p + off, (size_t) l, &h)) {
        free(h.key);
        free(h.value);
        return false;
      }
      f->headers[f->headers_count++] = h;
      off += (size_t) l;
    } else if (fn == 7 && wt == 2) {
      uint64_t l = read_varint(p, n, &off);
      if (off + l > n) return false;
      free(f->payload_type);
      f->payload_type = (char *) malloc((size_t) l + 1);
      if (!f->payload_type) return false;
      memcpy(f->payload_type, p + off, (size_t) l);
      f->payload_type[l] = 0;
      off += (size_t) l;
    } else if (fn == 8 && wt == 2) {
      uint64_t l = read_varint(p, n, &off);
      if (off + l > n) return false;
      free(f->payload);
      f->payload = (unsigned char *) malloc((size_t) l);
      if (l > 0 && !f->payload) return false;
      if (l > 0) memcpy(f->payload, p + off, (size_t) l);
      f->payload_len = (size_t) l;
      off += (size_t) l;
    } else {
      skip_field(p, n, &off, wt);
    }
  }
  return true;
}

static bool encode_frame(const WSFrame *f, ByteBuf *out) {
  size_t i;
  if (!pb_add_varint_field(out, 1, f->seq_id)) return false;
  if (!pb_add_varint_field(out, 2, f->log_id)) return false;
  if (!pb_add_varint_field(out, 3, (uint64_t) f->service)) return false;
  if (!pb_add_varint_field(out, 4, (uint64_t) f->method)) return false;
  for (i = 0; i < f->headers_count; i++) {
    ByteBuf hb = {0};
    if (!f->headers[i].key || !f->headers[i].value) continue;
    if (!pb_add_bytes(&hb, 1, f->headers[i].key, strlen(f->headers[i].key)) ||
        !pb_add_bytes(&hb, 2, f->headers[i].value,
                      strlen(f->headers[i].value)) ||
        !pb_add_bytes(out, 5, hb.data, hb.len)) {
      free(hb.data);
      return false;
    }
    free(hb.data);
  }
  if (f->payload_type &&
      !pb_add_bytes(out, 7, f->payload_type, strlen(f->payload_type)))
    return false;
  if (!pb_add_bytes(out, 8, f->payload, f->payload_len)) return false;
  return true;
}

static void free_pending(PendingMessage *p) {
  int i;
  if (!p) return;
  free(p->msg_id);
  p->msg_id = NULL;
  if (p->parts) {
    for (i = 0; i < p->sum; i++) free(p->parts[i]);
    free(p->parts);
    p->parts = NULL;
  }
  free(p->part_lens);
  p->part_lens = NULL;
  p->sum = 0;
  p->created_ms = 0;
}

static char *combine_payload(FeishuWS *ws, const char *msg_id, int sum, int seq,
                             const unsigned char *p, size_t n) {
  int i;
  PendingMessage *slot = NULL;
  if (!msg_id || sum <= 1 || seq < 0 || seq >= sum) return NULL;
  for (i = 0; i < 16; i++) {
    if (ws->pending[i].msg_id && strcmp(ws->pending[i].msg_id, msg_id) == 0) {
      slot = &ws->pending[i];
      break;
    }
  }
  if (!slot) {
    for (i = 0; i < 16; i++) {
      if (!ws->pending[i].msg_id) {
        slot = &ws->pending[i];
        break;
      }
    }
  }
  if (!slot) slot = &ws->pending[0];
  if (!slot->msg_id || strcmp(slot->msg_id, msg_id) != 0 || slot->sum != sum) {
    free_pending(slot);
    slot->msg_id = strdup(msg_id);
    slot->sum = sum;
    slot->parts = (char **) calloc((size_t) sum, sizeof(char *));
    slot->part_lens = (size_t *) calloc((size_t) sum, sizeof(size_t));
    slot->created_ms = now_ms();
    if (!slot->msg_id || !slot->parts || !slot->part_lens) {
      free_pending(slot);
      return NULL;
    }
  }
  free(slot->parts[seq]);
  slot->parts[seq] = (char *) malloc(n + 1);
  if (!slot->parts[seq]) return NULL;
  memcpy(slot->parts[seq], p, n);
  slot->parts[seq][n] = 0;
  slot->part_lens[seq] = n;
  {
    size_t total = 0;
    char *all;
    for (i = 0; i < sum; i++) {
      if (!slot->parts[i]) return NULL;
      total += slot->part_lens[i];
    }
    all = (char *) malloc(total + 1);
    if (!all) return NULL;
    total = 0;
    for (i = 0; i < sum; i++) {
      memcpy(all + total, slot->parts[i], slot->part_lens[i]);
      total += slot->part_lens[i];
    }
    all[total] = 0;
    free_pending(slot);
    return all;
  }
}

static void cleanup_pending_expired(FeishuWS *ws) {
  int i;
  uint64_t t = now_ms();
  for (i = 0; i < 16; i++) {
    if (ws->pending[i].msg_id && t > ws->pending[i].created_ms + 5000)
      free_pending(&ws->pending[i]);
  }
}

static void feishu_ws_send_frame(struct mg_connection *c, WSFrame *f) {
  ByteBuf b = {0};
  if (!encode_frame(f, &b)) {
    free(b.data);
    return;
  }
  mg_ws_send(c, (const char *) b.data, b.len, WEBSOCKET_OP_BINARY);
  free(b.data);
}

static void feishu_ws_send_ping(struct mg_connection *c, FeishuWS *ws) {
  WSFrame f;
  memset(&f, 0, sizeof(f));
  f.method = 0;
  f.service = ws->service_id;
  frame_header_set(&f, "type", "ping");
  feishu_ws_send_frame(c, &f);
  frame_free(&f);
}

static void feishu_ws_send_ack(struct mg_connection *c, WSFrame *in, int code,
                               long biz_rt_ms, const char *data_text) {
  WSFrame out;
  cJSON *payload = cJSON_CreateObject();
  cJSON *headers = cJSON_CreateObject();
  char *pstr;
  char tmp[32];
  size_t i;
  memset(&out, 0, sizeof(out));
  out.seq_id = in->seq_id;
  out.log_id = in->log_id;
  out.service = in->service;
  out.method = in->method;
  out.payload_type = in->payload_type ? strdup(in->payload_type) : NULL;
  for (i = 0; i < in->headers_count; i++) {
    if (in->headers[i].key && in->headers[i].value)
      frame_header_set(&out, in->headers[i].key, in->headers[i].value);
  }
  snprintf(tmp, sizeof(tmp), "%ld", biz_rt_ms);
  frame_header_set(&out, "biz_rt", tmp);
  cJSON_AddNumberToObject(payload, "code", code);
  cJSON_AddItemToObject(payload, "headers", headers);
  cJSON_AddStringToObject(payload, "data", data_text ? data_text : "");
  pstr = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  if (pstr) {
    out.payload = (unsigned char *) pstr;
    out.payload_len = strlen(pstr);
    feishu_ws_send_frame(c, &out);
  }
  frame_free(&out);
}

static bool is_duplicate_event(FeishuWS *ws, const char *event_id) {
  int i;
  if (!event_id) return false;
  for (i = 0; i < 20; i++) {
    if (ws->recent_msg_ids[i] &&
        strcmp(ws->recent_msg_ids[i], event_id) == 0)
      return true;
  }
  if (ws->recent_msg_ids[ws->msg_id_idx]) free(ws->recent_msg_ids[ws->msg_id_idx]);
  ws->recent_msg_ids[ws->msg_id_idx] = strdup(event_id);
  ws->msg_id_idx = (ws->msg_id_idx + 1) % 20;
  return false;
}

static void handle_event_json(FeishuWS *ws, const char *event_json) {
  cJSON *root, *header, *event_type, *event_id;
  cJSON *event, *message, *sender, *chat_id, *content, *sender_id_obj, *open_id, *user_id, *union_id;
  const char *sender_id = NULL;
  char *text = NULL;
  if (!event_json) return;
  root = cJSON_Parse(event_json);
  if (!root) return;
  header = cJSON_GetObjectItem(root, "header");
  event_type = header ? cJSON_GetObjectItem(header, "event_type") : NULL;
  event_id = header ? cJSON_GetObjectItem(header, "event_id") : NULL;
  if (event_id && cJSON_IsString(event_id) &&
      is_duplicate_event(ws, event_id->valuestring)) {
    cJSON_Delete(root);
    return;
  }
  if (!event_type || !cJSON_IsString(event_type) ||
      strcmp(event_type->valuestring, "im.message.receive_v1") != 0) {
    cJSON_Delete(root);
    return;
  }
  event = cJSON_GetObjectItem(root, "event");
  message = event ? cJSON_GetObjectItem(event, "message") : NULL;
  sender = event ? cJSON_GetObjectItem(event, "sender") : NULL;
  chat_id = message ? cJSON_GetObjectItem(message, "chat_id") : NULL;
  content = message ? cJSON_GetObjectItem(message, "content") : NULL;
  sender_id_obj = sender ? cJSON_GetObjectItem(sender, "sender_id") : NULL;
  open_id = sender_id_obj ? cJSON_GetObjectItem(sender_id_obj, "open_id") : NULL;
  user_id = sender_id_obj ? cJSON_GetObjectItem(sender_id_obj, "user_id") : NULL;
  union_id = sender_id_obj ? cJSON_GetObjectItem(sender_id_obj, "union_id") : NULL;
  if (open_id && cJSON_IsString(open_id) && open_id->valuestring) sender_id = open_id->valuestring;
  else if (user_id && cJSON_IsString(user_id) && user_id->valuestring) sender_id = user_id->valuestring;
  else if (union_id && cJSON_IsString(union_id) && union_id->valuestring) sender_id = union_id->valuestring;
  if (chat_id && content && ws->callback && cJSON_IsString(chat_id) &&
      cJSON_IsString(content) && sender_id) {
    cJSON *inner = cJSON_Parse(content->valuestring);
    if (inner) {
      cJSON *txt = cJSON_GetObjectItem(inner, "text");
      if (txt && cJSON_IsString(txt) && txt->valuestring)
        text = strdup(txt->valuestring);
      cJSON_Delete(inner);
    }
    if (!text && content->valuestring) text = strdup(content->valuestring);
    if (text) {
      ws->callback(chat_id->valuestring, text, sender_id, ws->user_data);
      free(text);
    }
  }
  cJSON_Delete(root);
}

static void ws_handler(struct mg_connection *c, int ev, void *ev_data) {
  FeishuWS *ws = (FeishuWS *) c->fn_data;
  if (ev == MG_EV_WS_OPEN) {
    ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
  } else if (ev == MG_EV_WS_MSG) {
    WSFrame f;
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    char *payload_text = NULL;
    int status_code = 200;
    long start_ms = (long) now_ms();
    memset(&f, 0, sizeof(f));
    if (!parse_frame((const unsigned char *) wm->data.buf, wm->data.len, &f)) {
      frame_free(&f);
      return;
    }
    if (f.method == 0) {
      const char *t = frame_header_get(&f, "type");
      if (t && strcmp(t, "pong") == 0 && f.payload && f.payload_len > 0) {
        cJSON *conf =
            cJSON_ParseWithLength((const char *) f.payload, f.payload_len);
        if (conf) {
          cJSON *pi = cJSON_GetObjectItem(conf, "PingInterval");
          if (cJSON_IsNumber(pi) && pi->valueint > 0)
            ws->ping_interval_ms = (uint64_t) pi->valueint * 1000;
          cJSON_Delete(conf);
        }
      }
      frame_free(&f);
      return;
    }
    if (f.method != 1) {
      frame_free(&f);
      return;
    }
    {
      int sum = frame_header_get_int(&f, "sum");
      int seq = frame_header_get_int(&f, "seq");
      const char *msg_id = frame_header_get(&f, "message_id");
      if (sum > 1) {
        payload_text =
            combine_payload(ws, msg_id, sum, seq, f.payload, f.payload_len);
      } else if (f.payload && f.payload_len > 0) {
        payload_text = (char *) malloc(f.payload_len + 1);
        if (payload_text) {
          memcpy(payload_text, f.payload, f.payload_len);
          payload_text[f.payload_len] = 0;
        }
      }
    }
    if (payload_text) {
      handle_event_json(ws, payload_text);
      free(payload_text);
    } else if (frame_header_get_int(&f, "sum") > 1) {
      frame_free(&f);
      return;
    } else {
      status_code = 500;
    }
    feishu_ws_send_ack(c, &f, status_code, (long) (now_ms() - (uint64_t) start_ms),
                       "");
    frame_free(&f);
  } else if (ev == MG_EV_CLOSE) {
    ws->running = false;
    log_info("[Feishu] WS Closed");
  }
}

FeishuWS *feishu_ws_create() {
  FeishuWS *ws = (FeishuWS *) malloc(sizeof(FeishuWS));
  if (!ws) return NULL;
  memset(ws, 0, sizeof(*ws));
  mg_mgr_init(&ws->mgr);
  ws->ping_interval_ms = 120000;
  ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
  return ws;
}

void feishu_ws_destroy(FeishuWS *ws) {
  int i;
  if (!ws) return;
  mg_mgr_free(&ws->mgr);
  for (i = 0; i < 20; i++) free(ws->recent_msg_ids[i]);
  for (i = 0; i < 16; i++) free_pending(&ws->pending[i]);
  free(ws);
}

bool feishu_ws_connect(FeishuWS *ws, const char *url) {
  const char *sid;
  if (!ws || !url) return false;
  ws->c = mg_ws_connect(&ws->mgr, url, ws_handler, ws, NULL);
  if (!ws->c) {
    log_error("[Feishu] Connection failed");
    return false;
  }
  {
    struct mg_str host = mg_url_host(url);
    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    if (mg_url_is_ssl(url)) mg_tls_init(ws->c, &opts);
  }
  sid = strstr(url, "service_id=");
  if (sid) ws->service_id = (int32_t) atoi(sid + strlen("service_id="));
  return true;
}

void feishu_ws_stop(FeishuWS *ws) {
  if (ws) ws->running = false;
}

void feishu_ws_run(FeishuWS *ws, FeishuWSMessageCallback callback,
                   void *user_data) {
  if (!ws) return;
  ws->callback = callback;
  ws->user_data = user_data;
  ws->running = true;
  while (ws->running && ws->mgr.conns) {
    mg_mgr_poll(&ws->mgr, 100);
    cleanup_pending_expired(ws);
    if (ws->c && now_ms() >= ws->next_ping_ms) {
      feishu_ws_send_ping(ws->c, ws);
      ws->next_ping_ms = now_ms() + ws->ping_interval_ms;
    }
  }
}
