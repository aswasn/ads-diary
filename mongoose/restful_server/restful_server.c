/*
 * Copyright (c) 2014 Cesanta Software Limited
 * All rights reserved
 */

#include "mongoose.h"
#include <hiredis.h>
#include <zmq.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>

typedef void* thread_func_t (void *);

static const char *s_http_port = "8000";
static struct mg_serve_http_opts s_http_server_opts;

static redisContext *redis_cli;
static void *zmq_ctx;

#define KEY_LEN 32


typedef struct {
    uint32_t msg_len;    // aligened to 8 bytes

    struct msg_body {
        uint32_t value_len;
        char key[KEY_LEN];
        char *value;
    } body;
} __attribute__((packed)) replicate_msg_t;


typedef struct {
    pthread_t tid;
    void *cli_sock;
    void *srv_sock;
    thread_func_t* process;
    char remote_host[32];
    int remote_port;
} replicater_t;

void redis_init()
{
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    char *hostname = "127.0.0.1";
    int port = 6379;
    redis_cli = redisConnectWithTimeout(hostname, port, timeout);
    if (redis_cli == NULL || redis_cli->err) {
        if (redis_cli) {
            printf("Connection error: %s\n", redis_cli->errstr);
            redisFree(redis_cli);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }
}

void *replicater_thread(void *arg)
{
    replicater_t *r = (replicater_t *)arg;
    assert(r->cli_sock != NULL);
    assert(r->srv_sock != NULL);
    uint32_t msg_len, data_len;
    struct msg_body *msg;
    char *buf;

    int rc;
    while (1) {
        msg_len = 0;
        rc = zmq_recv(r->srv_sock, &msg_len, sizeof(msg_len), 0);
        if (rc == -1) {
            perror("replicater: zmq_recv msg_len");
            exit(1);
        }
        data_len = msg_len - sizeof(msg_len);

        buf = (char*) malloc(data_len);
        rc = zmq_recv(r->srv_sock, buf, sizeof(struct msg_body), 0);
        if (rc == -1) {
            perror("replicater: zmq_recv payload");
            exit(1);
        }
        msg = (struct msg_body *)buf;
        assert(data_len == msg->value_len + KEY_LEN);


        printf("replicater: received message[msg_len:%d, value_len:%d, value:%s]\n",
                msg_len, msg->value_len, msg->value);


        free(buf);
    }

}

void replicater_init(replicater_t *r)
{
    zmq_ctx = zmq_ctx_new();
    r->cli_sock = zmq_socket(zmq_ctx, ZMQ_REQ);
    r->srv_sock = zmq_socket(zmq_ctx, ZMQ_REP);
    r->process = replicater_thread;
    r->remote_port = 5555;

    char remote_addr[48];
    int rc;
    sprintf(remote_addr, "tcp:://%s:%d", r->remote_host, r->remote_port);
    // client sock
    printf("DEBUG: remote_addr: %s\n", remote_addr);

    rc = zmq_connect(r->cli_sock, remote_addr);
    assert(rc == 0);
    rc = zmq_bind(r->srv_sock, "tcp://127.0.0.1:5555");
    assert(rc == 0);
    printf("DEBUG: replicater_init success\n");
}

static void handle_sum_call(struct mg_connection *nc, struct http_message *hm) {
  char n1[100], n2[100];
  double result;
  redisReply *reply;

  /* Get form variables */
  mg_get_http_var(&hm->body, "n1", n1, sizeof(n1));
  mg_get_http_var(&hm->body, "n2", n2, sizeof(n2));

  /* Send headers */
  mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");

  /* Compute the result and send it back as a JSON object */
  result = strtod(n1, NULL) + strtod(n2, NULL);

  reply = redisCommand(redis_cli,"SET %s %d", "sum", (int)result);
  printf("REDIS_CLI: SET: %s\n", reply->str);
  freeReplyObject(reply);

  mg_printf_http_chunk(nc, "{ \"result\": %lf }", result);
  mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

// api/get_diary_content, read from local redis
static void get_diary_content(struct mg_connection *nc, struct http_message *hm) {
  char diary_id[100];
  redisReply *reply;

  /* Get form variables */
  mg_get_http_var(&hm->body, "diary_id", diary_id, sizeof(diary_id));

  /* Send headers */
  mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");

  /* Compute the result and send it back as a JSON object */
  reply = redisCommand(redis_cli, "GET %s %s", "diary", (int)result);
  printf("REDIS_CLI: SET: %s\n", reply->str);
  freeReplyObject(reply);

  mg_printf_http_chunk(nc, "{ \"result\": %lf }", result);
  mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;

  switch (ev) {
    case MG_EV_HTTP_REQUEST:
      if (mg_vcmp(&hm->uri, "/api/v1/sum") == 0) {
        handle_sum_call(nc, hm); /* Handle RESTful call */
      } else if (mg_vcmp(&hm->uri, "/api/get_diary_content") == 0) {
        get_diary_content(nc, hm); /* Handle RESTful call */
      } else if (mg_vcmp(&hm->uri, "/printcontent") == 0) {
        char buf[100] = {0};
        memcpy(buf, hm->body.p,
               sizeof(buf) - 1 < hm->body.len ? sizeof(buf) - 1 : hm->body.len);
        printf("%s\n", buf);
      } else {
        mg_serve_http(nc, hm, s_http_server_opts); /* Serve static content */
      }
      break;
    default:
      break;
  }
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;
  struct mg_connection *nc;
  struct mg_bind_opts bind_opts;
  int i;
  char *cp;
  const char *err_str;
#if MG_ENABLE_SSL
  const char *ssl_cert = NULL;
#endif
  redisReply *reply;
  replicater_t replicater;

  mg_mgr_init(&mgr, NULL);
  redis_init();


  /* PING server */
  reply = redisCommand(redis_cli,"PING");
  printf("REDIS_CLI: PING: %s\n", reply->str);
  freeReplyObject(reply);

  /* Use current binary directory as document root */
  if (argc > 0 && ((cp = strrchr(argv[0], DIRSEP)) != NULL)) {
    *cp = '\0';
    s_http_server_opts.document_root = argv[0];
  }

  /* Process command line options to customize HTTP server */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      mgr.hexdump_file = argv[++i];
    } else if (strcmp(argv[i], "--remote_host") == 0 && i + 1 < argc) {
        strcmp(replicater.remote_host, argv[++i]);
    // } else if (strcmp(argv[i], "--remote_port") == 0 && i + 1 < argc) {
        // replicater.remote_port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      s_http_server_opts.document_root = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      s_http_port = argv[++i];
    } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
      s_http_server_opts.auth_domain = argv[++i];
    } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
      s_http_server_opts.global_auth_file = argv[++i];
    } else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
      s_http_server_opts.per_directory_auth_file = argv[++i];
    } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
      s_http_server_opts.url_rewrites = argv[++i];
#if MG_ENABLE_HTTP_CGI
    } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      s_http_server_opts.cgi_interpreter = argv[++i];
#endif
#if MG_ENABLE_SSL
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      ssl_cert = argv[++i];
#endif
    } else {
      fprintf(stderr, "Unknown option: [%s]\n", argv[i]);
      exit(1);
    }
  }


  printf("calling replicater_init...\n");
  replicater_init(&replicater);
  pthread_create(&(replicater.tid), NULL, replicater.process, &replicater);
  printf("replicater created!\n");


  /* Set HTTP server options */
  memset(&bind_opts, 0, sizeof(bind_opts));
  bind_opts.error_string = &err_str;
#if MG_ENABLE_SSL
  if (ssl_cert != NULL) {
    bind_opts.ssl_cert = ssl_cert;
  }
#endif
  nc = mg_bind_opt(&mgr, s_http_port, ev_handler, bind_opts);
  if (nc == NULL) {
    fprintf(stderr, "Error starting server on port %s: %s\n", s_http_port,
            *bind_opts.error_string);
    exit(1);
  }

  mg_set_protocol_http_websocket(nc);
  s_http_server_opts.enable_directory_listing = "yes";

  printf("Starting RESTful server on port %s, serving %s\n", s_http_port,
         s_http_server_opts.document_root);
  for (;;) {
    mg_mgr_poll(&mgr, 1000);
  }

  pthread_join(replicater.tid, NULL);
  mg_mgr_free(&mgr);

  return 0;
}
