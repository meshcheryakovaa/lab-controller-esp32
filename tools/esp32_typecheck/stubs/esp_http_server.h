// Declaration-only esp_http_server, enough for PsychicHttp's headers and for
// the firmware that uses them.  See tools/esp32_typecheck/README.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// --- http_parser ------------------------------------------------------------
typedef enum {
  HTTP_DELETE = 0,
  HTTP_GET,
  HTTP_HEAD,
  HTTP_POST,
  HTTP_PUT,
  HTTP_CONNECT,
  HTTP_OPTIONS,
  HTTP_TRACE,
  HTTP_PATCH
} http_method;

const char* http_method_str(http_method m);

// --- server -----------------------------------------------------------------
typedef void* httpd_handle_t;

typedef enum {
  HTTPD_500_INTERNAL_SERVER_ERROR,
  HTTPD_501_METHOD_NOT_IMPLEMENTED,
  HTTPD_505_VERSION_NOT_SUPPORTED,
  HTTPD_400_BAD_REQUEST,
  HTTPD_401_UNAUTHORIZED,
  HTTPD_403_FORBIDDEN,
  HTTPD_404_NOT_FOUND,
  HTTPD_405_METHOD_NOT_ALLOWED,
  HTTPD_408_REQ_TIMEOUT,
  HTTPD_411_LENGTH_REQUIRED,
  HTTPD_414_URI_TOO_LONG,
  HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE
} httpd_err_code_t;

struct httpd_req_t {
  httpd_handle_t handle;
  int method;
  char uri[512];
  std::size_t content_len;
  void* aux;
  void* user_ctx;
  bool sess_ctx_changed;
  void* sess_ctx;
  void (*free_ctx)(void* ctx);
  bool ignore_sess_ctx_changes;
};

typedef bool (*httpd_uri_match_func_t)(const char* uri_template,
                                       const char* uri_to_match,
                                       std::size_t match_upto);

struct httpd_uri_t {
  const char* uri;
  int method;
  esp_err_t (*handler)(httpd_req_t* r);
  void* user_ctx;
  bool is_websocket;
  bool handle_ws_control_frames;
  const char* supported_subprotocol;
};

struct httpd_config_t {
  unsigned task_priority;
  std::size_t stack_size;
  int core_id;
  std::uint16_t server_port;
  std::uint16_t ctrl_port;
  std::uint16_t max_open_sockets;
  std::uint16_t max_uri_handlers;
  std::uint16_t max_resp_headers;
  std::uint16_t backlog_conn;
  bool lru_purge_enable;
  std::uint16_t recv_wait_timeout;
  std::uint16_t send_wait_timeout;
  void* global_user_ctx;
  void (*global_user_ctx_free_fn)(void* ctx);
  void* global_transport_ctx;
  void (*global_transport_ctx_free_fn)(void* ctx);
  bool enable_so_linger;
  int linger_timeout;
  bool keep_alive_enable;
  int keep_alive_idle;
  int keep_alive_interval;
  int keep_alive_count;
  void* (*open_fn)(httpd_handle_t hd, int sockfd);
  void (*close_fn)(httpd_handle_t hd, int sockfd);
  httpd_uri_match_func_t uri_match_fn;
};

#define HTTPD_DEFAULT_CONFIG() (httpd_config_t{})

#define HTTPD_SOCK_ERR_FAIL (-1)
#define HTTPD_SOCK_ERR_INVALID (-2)
#define HTTPD_SOCK_ERR_TIMEOUT (-3)

#define HTTPD_RESP_USE_STRLEN ((std::ptrdiff_t)-1)

extern "C" {
esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                     const httpd_uri_t* uri_handler);
esp_err_t httpd_unregister_uri_handler(httpd_handle_t handle, const char* uri,
                                       int method);
esp_err_t httpd_register_err_handler(httpd_handle_t handle,
                                     httpd_err_code_t error,
                                     esp_err_t (*handler)(httpd_req_t* req,
                                                          httpd_err_code_t err));

int httpd_req_recv(httpd_req_t* r, char* buf, std::size_t buf_len);
std::size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* field,
                                      char* val, std::size_t val_size);
std::size_t httpd_req_get_url_query_len(httpd_req_t* r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf,
                                      std::size_t buf_len);
esp_err_t httpd_req_get_cookie_val(httpd_req_t* req, const char* cookie_name,
                                   char* val, std::size_t* val_size);
int httpd_req_to_sockfd(httpd_req_t* r);

esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, std::ptrdiff_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t* r, const char* buf,
                                std::ptrdiff_t buf_len);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t* r, const char* str);
esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status);
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type);
esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value);
esp_err_t httpd_resp_send_err(httpd_req_t* req, httpd_err_code_t error,
                              const char* msg);

void* httpd_sess_get_ctx(httpd_handle_t handle, int sockfd);
void httpd_sess_set_ctx(httpd_handle_t handle, int sockfd, void* ctx,
                        void (*free_fn)(void* ctx));
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd);
esp_err_t httpd_get_client_list(httpd_handle_t handle, std::size_t* fds,
                                int* client_fds);
esp_err_t httpd_queue_work(httpd_handle_t handle, void (*work)(void* arg),
                           void* arg);

bool httpd_uri_match_wildcard(const char* uri_template, const char* uri_to_match,
                              std::size_t match_upto);
}

// Async transfer completion callback (ESP-IDF 5.x).
typedef void (*transfer_complete_cb)(esp_err_t err, int socket, void* arg);

// --- websockets -------------------------------------------------------------
typedef enum {
  HTTPD_WS_TYPE_CONTINUE = 0x0,
  HTTPD_WS_TYPE_TEXT = 0x1,
  HTTPD_WS_TYPE_BINARY = 0x2,
  HTTPD_WS_TYPE_CLOSE = 0x8,
  HTTPD_WS_TYPE_PING = 0x9,
  HTTPD_WS_TYPE_PONG = 0xA
} httpd_ws_type_t;

typedef struct httpd_ws_frame {
  bool final;
  bool fragmented;
  httpd_ws_type_t type;
  std::uint8_t* payload;
  std::size_t len;
} httpd_ws_frame_t;

extern "C" {
esp_err_t httpd_ws_recv_frame(httpd_req_t* req, httpd_ws_frame_t* pkt,
                              std::size_t max_len);
esp_err_t httpd_ws_send_frame(httpd_req_t* req, httpd_ws_frame_t* pkt);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t hd, int fd,
                                    httpd_ws_frame_t* frame);
esp_err_t httpd_ws_send_data_async(httpd_handle_t handle, int socket,
                                   httpd_ws_frame_t* frame,
                                   void (*callback)(esp_err_t err, int socket,
                                                    void* arg),
                                   void* arg);
esp_err_t httpd_ws_get_frame_type(httpd_req_t* req, httpd_ws_frame_t* frame);
}
