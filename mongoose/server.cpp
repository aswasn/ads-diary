#include "mongoose.h"
#include "object.hpp"
#include <hiredis.h>
#include <zmq.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <list>
#include <sstream>
#include <vector>
#include "timer.hpp"

typedef void* thread_func_t (void *);
#define REDIS_COMMAND (redisReply *)redisCommand

const char *success_str = "{ \"success\": 1 }";
const char *fail_str = "{ \"success\": 0 }";

static const char *s_http_port = "8000";
static struct mg_serve_http_opts s_http_server_opts;

static redisContext *redis_cli;
static redisContext *redis_cli_master;
static void *zmq_ctx;

#define MAX_KEY_LEN 32
#define MAX_VALUE_LEN 2048

#define SYNC_REPLICA do {reply = REDIS_COMMAND(redis_cli, "wait 1 0"); freeReplyObject(reply);} while(0);

static char key_[MAX_KEY_LEN];
static char value_[MAX_VALUE_LEN];
static bool global_slave = false;
static bool psi_mode = false;

static int site_id;

struct rep_msg_t {
    uint32_t key_len;
    uint32_t value_len;
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
} __attribute__((packed));


struct replicater_t {
    pthread_t tid;
    void *cli_sock;
    void *srv_sock;
    thread_func_t* process;
    char remote_host[32];
    int remote_port;
    int local_port;
    zmq_pollitem_t poll_items[1];
};


pthread_mutex_t mutex;
std::list<rep_msg_t> msg_list;


void redis_init(char *remote_host)
{
    redisReply *reply;
    struct timeval timeout = { 60, 500000 }; // 1.5 seconds
    char *local_host = "127.0.0.1";
    const int PORT = 6379;
    redis_cli = redisConnectWithTimeout(local_host, PORT, timeout);
    if (redis_cli == NULL || redis_cli->err) {
        if (redis_cli) {
            printf("Connection error: %s\n", redis_cli->errstr);
            redisFree(redis_cli);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }

    // I am slave
    if (global_slave) {

        redis_cli_master = redisConnectWithTimeout(remote_host, PORT, timeout);
        if (redis_cli_master == NULL || redis_cli_master->err) {
            if (redis_cli_master) {
                printf("Connection error: %s\n", redis_cli_master->errstr);
                redisFree(redis_cli_master);
            } else {
                printf("Connection error: can't allocate redis context\n");
            }
            exit(1);
        }

        reply = REDIS_COMMAND(redis_cli, "slaveof %s %d", remote_host, PORT);
        if (reply->type == REDIS_REPLY_ERROR) {
            printf("ERROR: slaveof %s:%d fail!\n", remote_host, PORT);
            assert(false);
        }
        freeReplyObject(reply);


        reply = REDIS_COMMAND(redis_cli, "info replication");
        printf("INFO: %s\n", reply->str);
        freeReplyObject(reply);
    }

}

void *replicater_thread(void *arg)
{
    replicater_t *r = (replicater_t *)arg;
    assert(r->cli_sock != NULL);
    assert(r->srv_sock != NULL);
    char *buf;

    int rc;

    char remote_addr[48];
    char local_addr[48];

    sprintf(remote_addr, "tcp://%s:%d", r->remote_host, r->remote_port);
    sprintf(local_addr, "tcp://%s:%d", "127.0.0.1", r->local_port);
    printf("DEBUG: remote_addr: %s\n", remote_addr);
    printf("DEBUG: local_addr: %s\n", local_addr);

    rc = zmq_bind(r->srv_sock, local_addr);
    if (rc != 0) {
        perror("zmq_bind");
        assert(false);
    }

    rc = zmq_connect(r->cli_sock, remote_addr);
    assert(rc == 0);
    printf("DEBUG: sockets setup success\n");

    while (1) {
        zmq_poll(r->poll_items, 1, 100);

        if (r->poll_items[0].revents & ZMQ_POLLIN) {
            rep_msg_t rep_msg;

            rc = zmq_recv(r->srv_sock, &rep_msg, sizeof(rep_msg_t), 0);
            if (rc == -1) {
                perror("replicater: zmq_recv msg_len");
                exit(1);
            }
            printf("DEBUG: recved message[key_len:%d, value_len:%d, key: %s, value:%s]\n",
                    rep_msg.key_len, rep_msg.value_len, rep_msg.key, rep_msg.value);
        }

        pthread_mutex_lock(&mutex);
        while (!msg_list.empty()) {
            rep_msg_t rep_msg = msg_list.front();
            rc = zmq_send(r->cli_sock, &rep_msg, sizeof(rep_msg_t), 0);
            if (rc == -1) {
                perror("replicater: zmq_send msg");
                exit(1);
            }
            msg_list.pop_front();
        }
        pthread_mutex_unlock(&mutex);
    }

}

void replicater_init(replicater_t *r, char *remote_host, int remote_port, int local_port)
{
    zmq_ctx = zmq_ctx_new();
    r->cli_sock = zmq_socket(zmq_ctx, ZMQ_REQ);
    r->srv_sock = zmq_socket(zmq_ctx, ZMQ_REP);
    r->process = replicater_thread;
    if (strlen(remote_host) == 0)
        strncpy(r->remote_host, "127.0.0.1", 32);
    else
        strncpy(r->remote_host, remote_host, 32);

    r->remote_port = remote_port == 0 ? 8777 : remote_port;
    r->local_port = local_port == 0 ? 8666 : local_port;
    r->poll_items[0].socket = r->srv_sock;
    r->poll_items[0].events = ZMQ_POLLIN;
}

// api/get_diary_content, read from local redis
static void handle_get_diary_content(struct mg_connection *nc, struct http_message *hm) {
    char redis_d_id[100] = "diary_", d_id[100];
    redisReply *reply;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));

    strncat(redis_d_id, d_id, 10);

    reply = REDIS_COMMAND(redis_cli, "GET %s", redis_d_id);
    printf("REDIS: GET %s: %s\n", redis_d_id, reply->str);

    /* Send headers */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", reply->str);
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
    freeReplyObject(reply);
}


static void handle_edit_diary(struct mg_connection *nc, struct http_message *hm) {
    objects::diary d;
    std::stringstream ss;
    redisReply *reply;
    bool success = true;
    char diary_id[16], user[16], snapshot_ver[16], content[1024];
    char redis_d_id[34] = "diary_";
    mg_get_http_var(&hm->body, "diary_id", diary_id, sizeof(diary_id));
    mg_get_http_var(&hm->body, "user_id", user, sizeof(user));
    mg_get_http_var(&hm->body, "content", content, sizeof(content));
    mg_get_http_var(&hm->body, "snapshot_ver", snapshot_ver, sizeof(content));

    d.id = std::atoi(&diary_id[0]);
    d.ver = std::atoi(&snapshot_ver[0]);
    d.content = std::string(content);
    d.user = std::string(user);


    reply = REDIS_COMMAND(redis_cli, "GET diary_%d", d.id);
    printf("handle_edit_diary: REDIS GET result: %s\n", reply->str);
    // auto json_obj = json::parse(reply->str);

    objects::diary redis_d = json::parse(reply->str);
    freeReplyObject(reply);



    if (d.id == redis_d.id && d.ver == redis_d.ver) {
        redis_d.content = d.content;
        redis_d.ver += 1;
        strcat(redis_d_id, diary_id);

        json j;
        j = redis_d;

        redisContext *cli = (global_slave ? redis_cli_master : redis_cli);
        reply = REDIS_COMMAND(cli, "SET %s %s", redis_d_id, j.dump().c_str());
        freeReplyObject(reply);

        if (!psi_mode)
            SYNC_REPLICA;

    } else {
        success = false;
    }

    /* Send response */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, success ? success_str : fail_str);
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

static void handle_add_comment(struct mg_connection *nc, struct http_message *hm) {
    char redis_d_id[100] = "diary_", d_id[100], user_id[100], content[500];
    std::vector<objects::comment> new_list;
    redisReply *reply;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));
    mg_get_http_var(&hm->body, "user_id", user_id, sizeof(user_id));
    mg_get_http_var(&hm->body, "content", content, sizeof(content));

    strcat(redis_d_id, d_id);
    strcat(redis_d_id, "_comments");

    reply = REDIS_COMMAND(redis_cli, "GET %s", redis_d_id);
    if (reply->type != REDIS_REPLY_NIL) {
        std::vector<objects::comment> list = json::parse(reply->str);
        new_list = list;
    }
    freeReplyObject(reply);

    objects::comment new_comment = {-1, atoi(d_id), 0, user_id, content};

    new_list.push_back(new_comment);

    json j = new_list;


    redisContext *cli = (global_slave ? redis_cli_master : redis_cli);
    reply = REDIS_COMMAND(cli, "SET %s %s", redis_d_id, j.dump().c_str());
    freeReplyObject(reply);
    if (!psi_mode)
        SYNC_REPLICA;

    /* Send headers */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", "{ \"success\": 1 }");
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

static void handle_get_comments(struct mg_connection *nc, struct http_message *hm) {
    char redis_d_id[100] = "diary_", d_id[100];
    redisReply *reply;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));

    strcat(redis_d_id, d_id);
    strcat(redis_d_id, "_comments");

    reply = REDIS_COMMAND(redis_cli, "GET %s", redis_d_id);

    /* Send headers */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", reply->str);
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
    freeReplyObject(reply);
}

static void handle_get_like(struct mg_connection *nc, struct http_message *hm) {
    char redis_d_id[100] = "diary_", d_id[100];
    redisReply *reply;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));
    strcat(redis_d_id, d_id);
    strcat(redis_d_id, "_like");

    reply = REDIS_COMMAND(redis_cli, "GET %s", redis_d_id);
    objects::like like = {atoi(d_id), 0, 0};
    if (reply->str != NULL)
        like = json::parse(reply->str);

    /* Send headers */
    // like.num = reply->str == NULL ? 0 : atoi(reply->str);
    json j = like;
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", j.dump().c_str());
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
    freeReplyObject(reply);
}

static void handle_like(struct mg_connection *nc, struct http_message *hm) {
    char redis_d_id[100] = "diary_", d_id[100];
    char s_ver[16];
    redisReply *reply;
    bool success = true;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));
    mg_get_http_var(&hm->body, "snapshot_ver", s_ver, sizeof(s_ver));
    strcat(redis_d_id, d_id);
    strcat(redis_d_id, "_like");

    int diary_id = atoi(d_id);
    int snapshot_ver = atoi(s_ver);


    reply = REDIS_COMMAND(redis_cli, "GET %s", redis_d_id);
    objects::like redis_like = {diary_id, 0, 0};// = json.parse(reply->str);
    redisContext *cli = (global_slave ? redis_cli_master : redis_cli);
    if (reply->str == NULL) {
        // do nothing
    } else {
        redis_like = json::parse(reply->str);
    }

    if (diary_id == redis_like.diary_id && snapshot_ver == redis_like.ver) {
        redis_like.num += 1;
        redis_like.ver += 1;
        json j = redis_like;
        reply = REDIS_COMMAND(cli, "SET %s %s", redis_d_id, j.dump().c_str());
        freeReplyObject(reply);
        if (!psi_mode)
            SYNC_REPLICA;
    } else {
        success = false;
    }

    /* Send response */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", success ? success_str : fail_str);
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data) {
  struct http_message *hm = (struct http_message *) ev_data;

  switch (ev) {
    case MG_EV_HTTP_REQUEST:
      if (mg_vcmp(&hm->uri, "/api/get_diary_content") == 0) {
        handle_get_diary_content(nc, hm); /* Handle RESTful call */
      } else if (mg_vcmp(&hm->uri, "/api/edit_diary") == 0) {
        handle_edit_diary(nc, hm);
      } else if (mg_vcmp(&hm->uri, "/api/add_comment") == 0) {
        handle_add_comment(nc, hm);
      } else if (mg_vcmp(&hm->uri, "/api/get_comments") == 0) {
        handle_get_comments(nc, hm);
      } else if (mg_vcmp(&hm->uri, "/api/get_like") == 0) {
        handle_get_like(nc, hm);
      } else if (mg_vcmp(&hm->uri, "/api/like") == 0) {
        handle_like(nc, hm);
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


void generate_data();

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
  int remote_port = 0, local_port = 0;
  char remote_host[32] = {0};


  memset(&replicater, 0, sizeof(replicater));

  mg_mgr_init(&mgr, NULL);



  /* Use current binary directory as document root */
  if (argc > 0 && ((cp = strrchr(argv[0], DIRSEP)) != NULL)) {
    *cp = '\0';
    s_http_server_opts.document_root = argv[0];
  }

  /* Process command line options to customize HTTP server */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
      mgr.hexdump_file = argv[++i];
    } else if (strcmp(argv[i], "--rhost") == 0 && i + 1 < argc) {
        strcpy(remote_host, argv[++i]);
    } else if (strcmp(argv[i], "--rport") == 0 && i + 1 < argc) {
        remote_port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--lport") == 0 && i + 1 < argc) {
        local_port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--slave") == 0 && i + 1 < argc) {
        global_slave = true;
    } else if (strcmp(argv[i], "--psi") == 0 && i + 1 < argc) {
        psi_mode = true;
    } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
        strncpy(key_, argv[++i], MAX_KEY_LEN);
    } else if (strcmp(argv[i], "--value") == 0 && i + 1 < argc) {
        strncpy(value_, argv[++i], 1024);

    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      s_http_server_opts.document_root = argv[++i];
    } else if (strcmp(argv[i], "-siteid") == 0 && i + 1 < argc) {
      site_id = atoi(argv[++i]);
      printf("site id is %d.\n", site_id);
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      s_http_port = argv[++i];
    } else {
      fprintf(stderr, "Unknown option: [%s]\n", argv[i]);
      exit(1);
    }
  }

  redis_init(remote_host);

  /* PING server */
  reply = REDIS_COMMAND(redis_cli,"PING");
  printf("REDIS_CLI: PING: %s\n", reply->str);
  freeReplyObject(reply);

  /* Siyuan: 临时注释
   * replicater_init(&replicater, remote_host, remote_port, local_port);
   * pthread_create(&(replicater.tid), NULL, replicater.process, &replicater);
   * printf("replicater created!\n"); */


  // rep_msg_t rep_msg;
  // strncpy(rep_msg.key, key_, MAX_KEY_LEN);
  // rep_msg.key_len = strlen(rep_msg.key) + 1;
  // strncpy(rep_msg.value, value_, MAX_VALUE_LEN);
  // rep_msg.value_len = strlen(rep_msg.value) + 1;

  // msg_list.push_back(rep_msg);

  /* Set HTTP server options */
  memset(&bind_opts, 0, sizeof(bind_opts));
  bind_opts.error_string = &err_str;

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


  if (!global_slave)
      generate_data();

  // reply = REDIS_COMMAND(redis_cli, "GET %s", "diary_1");
  // printf("REDIS: GET %s: %s\n", "diary_1", reply->str);

  for (;;) {
    mg_mgr_poll(&mgr, 1000);
  }

  pthread_join(replicater.tid, NULL);
  mg_mgr_free(&mgr);

  return 0;
}


void generate_data()
{
    redisReply *reply;
    json d1 =  R"(
          {
            "id": 1,
            "ver": 0,
            "user": "bh",
            "content": "今天早上爸爸带我们全家去植物园，沿路上蝉声一直吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱……叫个不停，感觉很舒服",
            "utime": 0
          }
        )"_json;


    json d2 =  R"(
          {
            "id": 2,
            "ver": 0,
            "user": "bh",
            "content": "今天和妈妈去爬山，到了山顶，妈妈说安静的山间会有回音喔。我和表弟一起试着大叫━━“你好吗”果然大约3秒之后就听到：你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗（以下删去170多句“你好吗”）……一个字都没变，课本说的音波反射，我终于体会到了。真是有意义的一天。",
            "utime": 0
          }
        )"_json;

    json d3 =  R"(
          {
            "id": 2,
            "ver": 0,
            "user": "bh",
            "content": "还记得5天前的我，活蹦乱跳，如猴子一般。而因为显瘦，便连秋裤都没穿，但是现在呢，整天如企鹅一般，因为生病我连最爱的体育课都上不了，还真的是不能逞英雄，原本想逞英雄，如今成狗熊啊。再次跟大家提个醒，多穿点衣服哦，不然就如我一样，英雄变狗熊。",
            "utime": 0
          }
        )"_json;


    reply = REDIS_COMMAND(redis_cli, "SET %s %s", "diary_1", d1.dump().c_str());
    freeReplyObject(reply);

    reply = REDIS_COMMAND(redis_cli, "SET %s %s", "diary_2", d2.dump().c_str());
    freeReplyObject(reply);

    reply = REDIS_COMMAND(redis_cli, "SET %s %s", "diary_3", d3.dump().c_str());
    freeReplyObject(reply);

    std::vector<std::string> users = {"wn", "wsy", "bh"};
    std::vector<std::string> comments = {"这是评论1这是评论1这是评论1这是评论1这是评论1", "这是评论2这是评论2这是评论2这是评论2这是评论2",
        "这是评论3这是评论3这是评论3这是评论3", "这是评论4这是评论4这是评论4这是评论4"};

    /// comments
    json d1_comments;
    for (int i = 0; i < 5; ++i) {
        objects::comment com;
        com.id = i;
        com.diary_id = 1;
        com.ver = 0;
        com.user = users[random() % 3];
        com.content = comments[random() % 4];
        d1_comments.push_back(com);
    }

    reply = REDIS_COMMAND(redis_cli, "SET %s %s", "diary_1_comments", d1_comments.dump().c_str());
    freeReplyObject(reply);

    printf("generate_data: done!\n");

}
