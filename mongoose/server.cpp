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

#define PREFER_SITE(oid) (oid % 2)

typedef void* thread_func_t (void *);
#define REDIS_COMMAND (redisReply *)redisCommand

const char *success_str = "{ \"success\": 1 }";
const char *fail_str = "{ \"success\": 0 }";


static const char *s_http_port = "8000";
static struct mg_serve_http_opts s_http_server_opts;

redisContext *redis_cli;
redisContext *redis_cli_master;
void *zmq_ctx;

#define MAX_KEY_LEN 32
#define MAX_VALUE_LEN 2048

#define SYNC_REPLICA do {reply = REDIS_COMMAND(redis_cli, "wait 1 0"); freeReplyObject(reply);} while(0);

static char key_[MAX_KEY_LEN];
static char value_[MAX_VALUE_LEN];
static bool global_slave = false;
static bool psi_mode = false;

static int site_id;
pthread_mutex_t vts_lock = PTHREAD_MUTEX_INITIALIZER;
static int committedVTS[2] = {0, 0};

#define FAST_CMT 0x11
#define SLOW_CMT 0x22

struct rep_msg_t {
    rep_msg_t() {}
    rep_msg_t(int cmt_type, const char *k, const char *v, uint32_t k_l, uint32_t v_l) {
        commit_type = cmt_type;
        key_len = k_l;
        value_len = v_l;
        strncpy(key, k, k_l);
        strncpy(value, v, v_l);
    }
    int commit_type;
    uint32_t key_len = 0;
    uint32_t value_len = 0;
    char key[MAX_KEY_LEN] = {0};
    char value[MAX_VALUE_LEN] = {0};
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


pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::list<rep_msg_t> msg_list;

static bool fast_commit(psi_ver_t startTs, objects::object *obj, objects::obj_type obj_type);
static bool slow_commit(psi_ver_t startTs, objects::object *obj, objects::obj_type type);


bool acquire_lock(char *key)
{
    redisReply *reply;
    bool success;
    reply = REDIS_COMMAND(redis_cli, "INCR %s", key);
    if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
        success = true;
    } else {
        success = false;
    }
    freeReplyObject(reply);

    return success;
}

void release_lock(char *key)
{
    redisReply *reply;
    reply = REDIS_COMMAND(redis_cli, "SET %s 0", key);
    freeReplyObject(reply);
}

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

    reply = REDIS_COMMAND(redis_cli, "info replication");
    printf("INFO: %s\n", reply->str);
    freeReplyObject(reply);

}

void *replicater_thread(void *arg)
{
    redisReply *reply;
    replicater_t *r = (replicater_t *)arg;
    assert(r->cli_sock != NULL);
    assert(r->srv_sock != NULL);
    char *buf;
    int rc;
    char remote_addr[48];
    char local_addr[48];

    sprintf(remote_addr, "tcp://%s:%d", r->remote_host, r->remote_port);
    sprintf(local_addr, "tcp://%s:%d", "*", r->local_port);
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
    rc = zmq_setsockopt(r->cli_sock, ZMQ_SUBSCRIBE, "", 0);
    assert(rc == 0);

    while (1) {
        zmq_poll(r->poll_items, 1, 100);

        if (r->poll_items[0].revents & ZMQ_POLLIN) {
            rep_msg_t rep_msg;

            rc = zmq_recv(r->cli_sock, &rep_msg, sizeof(rep_msg_t), 0);
            if (rc == -1) {
                perror("replicater: srv_sock zmq_recv");
                exit(1);
            }

            // rc = zmq_send(r->srv_sock, "ok", 2, 0);
            // if (rc == -1) {
                // perror("replicater: srv_sock zmq_send");
                // exit(1);
            // }

            printf("DEBUG: Replicater: recved message[cmt_type: %d, key_len:%d, value_len:%d, key: %s, value:%s]\n",
                    rep_msg.commit_type, rep_msg.key_len, rep_msg.value_len, rep_msg.key, rep_msg.value);

            reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", rep_msg.key, rep_msg.value);
            freeReplyObject(reply);

            if (rep_msg.commit_type == SLOW_CMT) {
                char lock_key[48] = {0};
                sprintf(lock_key, "%s_lock", rep_msg.key);
                release_lock(lock_key);
            }

            pthread_mutex_lock(&vts_lock);
            committedVTS[1 - site_id] += 1;
            pthread_mutex_unlock(&vts_lock);
        }

        pthread_mutex_lock(&mutex);
        // printf("replicater: I am alive. msg_list.size(): %d\n", msg_list.size());
        char tmpbuf[10] = {0};
        while (!msg_list.empty()) {
            rep_msg_t rep_msg = msg_list.front();
            rc = zmq_send(r->srv_sock, &rep_msg, sizeof(rep_msg_t), 0);
            if (rc == -1) {
                // perror("replicater: cli_sock zmq_send");
                perror("replicater: publisher zmq_send");
                exit(1);
            }
            // rc = zmq_recv(r->cli_sock, tmpbuf, 10, 0);
            // if (rc == -1) {
                // perror("replicater: cli_sock zmq_recv");
                // exit(1);
            // }
            // printf("replicater: publisher recved: %s\n", tmpbuf);
            msg_list.pop_front();
        }
        pthread_mutex_unlock(&mutex);
    }

}

void replicater_init(replicater_t *r, char *remote_host, int remote_port, int local_port)
{
    zmq_ctx = zmq_ctx_new();
    r->cli_sock = zmq_socket(zmq_ctx, ZMQ_SUB);
    r->srv_sock = zmq_socket(zmq_ctx, ZMQ_PUB);
    // r->cli_sock = zmq_socket(zmq_ctx, ZMQ_REQ);
    // r->srv_sock = zmq_socket(zmq_ctx, ZMQ_REP);
    r->process = replicater_thread;
    if (strlen(remote_host) == 0)
        strncpy(r->remote_host, "127.0.0.1", 32);
    else
        strncpy(r->remote_host, remote_host, 32);

    r->remote_port = remote_port == 0 ? 8777 : remote_port;
    r->local_port = local_port == 0 ? 8666 : local_port;
    // r->poll_items[0].socket = r->srv_sock;
    r->poll_items[0].socket = r->cli_sock;
    r->poll_items[0].events = ZMQ_POLLIN;
}

// api/get_diary_content, read from local redis
static void handle_get_diary_content(struct mg_connection *nc, struct http_message *hm) {
    char hlist_id[100] = "diary_", d_id[100];
    redisReply *reply;
    std::vector<objects::diary> hlist;
    hlist.reserve(20);

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));

    strncat(hlist_id, d_id, 10);
    psi_ver_t max_ver(0,0);
    int max_idx = 0;

    reply = REDIS_COMMAND(redis_cli, "LRANGE %s 0 -1", hlist_id);
    if (reply->type != REDIS_REPLY_NIL) {
        for (int i = 0; i < reply->elements; i++) {
            objects::diary di = json::parse((reply->element[i])->str);
            if (di.ver.first >= max_ver.first && di.ver.second >= max_ver.second) {
                max_ver = di.ver;
                max_idx = i;
                hlist.push_back(di);
            }
        }
        freeReplyObject(reply);
    }

    objects::diary &latest_di = hlist[max_idx];
    json j = latest_di;

    printf("handle_get_diary_content: hlist_id: %s: latest_diary: %s\n", hlist_id, latest_di.content.c_str());
    /* Send headers */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", j.dump().c_str());
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}


static void handle_edit_diary(struct mg_connection *nc, struct http_message *hm) {
    objects::diary d;
    std::stringstream ss;
    redisReply *reply;
    bool success = true;
    char diary_id[16], user[16], ver1[16], ver2[16], content[1024];
    char redis_d_id[34] = "diary_";

    mg_get_http_var(&hm->body, "diary_id", diary_id, sizeof(diary_id));
    mg_get_http_var(&hm->body, "user_id", user, sizeof(user));
    mg_get_http_var(&hm->body, "content", content, sizeof(content));
    mg_get_http_var(&hm->body, "ver1", ver1, sizeof(ver1));
    mg_get_http_var(&hm->body, "ver2", ver2, sizeof(ver2));

    d.id = std::atoi(&diary_id[0]);
    d.ver = psi_ver_t(std::atoi(&ver1[0]), std::atoi(&ver2[0]));
    d.content = std::string(content);
    d.user = std::string(user);

    if (site_id == PREFER_SITE(d.id)) {
        success = fast_commit(d.ver, &d, objects::DIARY);
    } else {
        success = slow_commit(d.ver, &d, objects::DIARY);
    }

    /* Send response */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, success ? success_str : fail_str);
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
}

static void handle_add_comment(struct mg_connection *nc, struct http_message *hm) {
    char redis_d_id[100] = "diary_", d_id[100], user_id[100], content[500];
    redisReply *reply;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));
    mg_get_http_var(&hm->body, "user_id", user_id, sizeof(user_id));
    mg_get_http_var(&hm->body, "content", content, sizeof(content));

    objects::comment com(-1, psi_ver_t(0, 0), atoi(d_id), std::string(user_id), std::string(content));

    printf("handle_add_comment, user_id: %s, content: %s\n", user_id, content);
    bool result = fast_commit(psi_ver_t(0, 0), &com, objects::COMMENT);

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

    reply = REDIS_COMMAND(redis_cli, "LRANGE %s 0 -1", redis_d_id);

    std::string str = "[";
    if (reply->type != REDIS_REPLY_NIL) {
        for (int i = 0; i < reply->elements; i++) {
            str += (reply->element[i])->str;
            if (i != reply->elements - 1) str += ",";
        }
    }
    str += "]";

    printf("handle_get_comments, comments: %s\n", str.c_str());

    /* Send headers */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", str.c_str());
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


    int diary_id = atoi(d_id);

    if (PREFER_SITE(diary_id) == site_id) {
        reply = REDIS_COMMAND(redis_cli, "GET %s", redis_d_id);
    } else {
        reply = REDIS_COMMAND(redis_cli_master, "GET %s", redis_d_id);
    }
    int num = reply->str == NULL ? 0 : atoi(reply->str);

    json resp;
    resp["diary_id"] = diary_id;
    resp["num"] = num;

    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", resp.dump().c_str());
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
    freeReplyObject(reply);
}

static void handle_like(struct mg_connection *nc, struct http_message *hm) {
    char like_key[100] = "diary_", d_id[100];
    redisReply *reply;
    bool success;

    /* Get form variables */
    mg_get_http_var(&hm->body, "diary_id", d_id, sizeof(d_id));

    int diary_id = atoi(d_id);
    sprintf(like_key, "diary_%d_like", diary_id);

    if (PREFER_SITE(diary_id) == site_id) {
        reply = REDIS_COMMAND(redis_cli, "INCR %s", like_key);
    } else {
        reply = REDIS_COMMAND(redis_cli_master, "INCR %s", like_key);
    }

    json resp;
    resp["success"] = success ? 1 : 0;
    resp["num"] = reply->integer; //redis_like.num;

    /* Send response */
    mg_printf(nc, "%s", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    mg_printf_http_chunk(nc, "%s", resp.dump().c_str());
    mg_send_http_chunk(nc, "", 0); /* Send empty chunk, the end of response */
    freeReplyObject(reply);
}

/**
 * If this site is prefer site of the object, use fast commit
 */
static bool fast_commit(psi_ver_t startTs, objects::object *obj, objects::obj_type obj_type) {
    // std::vector<objects::diary> hlist_d;
    // std::vector<objects::comment> hlist_cm;
    redisReply *reply;
    bool success, rc;
    char history_key[32] = {0};
    char lock_key[48];

    if (obj_type == objects::DIARY) {
        sprintf(history_key, "diary_%d", obj->id);
        sprintf(lock_key, "%s_lock", history_key);
    } else {
        sprintf(history_key, "diary_%d_comments", ((objects::comment*)obj)->diary_id);
        sprintf(lock_key, "%s_lock", history_key);
    }
    printf("fast_commit: history_key: %s, lock_key: %s\n", history_key, lock_key);

    rc = acquire_lock(lock_key);
    if (!rc) {
        return false;
    }

    printf("fast_commit: acuire lock success!\n");

    if (obj_type == objects::DIARY) {
        // get object history
        reply = REDIS_COMMAND(redis_cli, "LRANGE %s 0 -1", history_key);
        if (reply->type != REDIS_REPLY_NIL) {
            // check whether objs in write-set have conflicts
            for (int i = 0; i < reply->elements; i++) {
                objects::object o = json::parse((reply->element[i])->str);
                if (startTs.first < o.ver.first || startTs.second < o.ver.second) {
                    freeReplyObject(reply);
                    printf("fast_commit: first check of conflict fail!\n");
                    // success = false;
                    release_lock(lock_key);
                    return false;
                }
            }
            freeReplyObject(reply);
        }
    }

    pthread_mutex_lock(&vts_lock);
    committedVTS[site_id]++;
    obj->ver = psi_ver_t(committedVTS[0], committedVTS[1]);
    pthread_mutex_unlock(&vts_lock);
    printf("fast_commit: committedVTS modified: [%d, %d]\n", committedVTS[0], committedVTS[1]);

    json j;
    if (objects::DIARY == obj_type)
        j = *((objects::diary*)obj);
    else if (objects::COMMENT == obj_type) {
        j = *((objects::comment*)obj);
    }

    std::string json_str = j.dump();
    printf("fast_commit: object to be commit: %s\n", j.dump().c_str());
    reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", history_key, json_str.c_str());
    freeReplyObject(reply);

    rep_msg_t msg(FAST_CMT, history_key, json_str.c_str(), strlen(history_key), json_str.length());
    printf("fast_commit: rep_msg: cmt_type:%x, key_len:%d, value_len:%d, key:%s, value:%s\n",
            msg.commit_type, msg.key_len, msg.value_len, msg.key, msg.value);
    pthread_mutex_lock(&mutex);
    msg_list.push_back(msg);
    pthread_mutex_unlock(&mutex);

    success = true;
    goto done;

done:
    release_lock(lock_key);
    return success;

}

static bool slow_commit(psi_ver_t startTs, objects::object *obj, objects::obj_type type) {
    printf("-slow_commit-enter slow_commit, psi_ver_t: <%d, %d>\n", startTs.first, startTs.second);
    redisReply *reply;
    if (type == objects::DIARY) {
        objects::diary diary = *((objects::diary*) obj);
        printf("-slow_commit-diary_id: %d, diary_ver1: %d, dairy_ver2: %d, diary_content: %s\n",
        diary.id, diary.ver.first, diary.ver.second, diary.content.c_str());
        char lock_name[100] = {0}, history_name[100] = {0};
        sprintf(lock_name, "diary_%d_lock", diary.id);
        sprintf(history_name, "diary_%d", diary.id);
        printf("-slow_commit-lock_name: %s, history_name: %s\n", lock_name, history_name);
        reply = REDIS_COMMAND(redis_cli_master, "INCR %s", lock_name);
        if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
            printf("-slow_commit-lock_value: %d\n", reply->integer);
            // we can check now
            freeReplyObject(reply);
            reply = REDIS_COMMAND(redis_cli_master, "LRANGE %s 0 -1", history_name);
            if (reply->type != REDIS_REPLY_NIL) {
                for (int i = 0; i < reply->elements; i++) {
                    objects::diary tmp_d = json::parse((reply->element[i])->str);
                    if (tmp_d.ver.first > startTs.first || tmp_d.ver.second > startTs.second) {
                        freeReplyObject(reply);
                        reply = REDIS_COMMAND(redis_cli_master, "SET %s 0", lock_name);
                        freeReplyObject(reply);
                        return false;
                    }
                }
                printf("-slow_commit-check no conflict\n");
                freeReplyObject(reply);
                pthread_mutex_lock(&vts_lock);
                committedVTS[site_id]++;
                diary.ver = psi_ver_t(committedVTS[0], committedVTS[1]);
                pthread_mutex_unlock(&vts_lock);
                json j = diary;
                std::string diary_json = j.dump();
                printf("-slow_commit-diary_json: %s\n", diary_json.c_str());
                reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", history_name, diary_json.c_str());
                freeReplyObject(reply);
                rep_msg_t msg(SLOW_CMT, history_name, diary_json.c_str(), strlen(history_name), diary_json.length());
                pthread_mutex_lock(&mutex);
                msg_list.push_back(msg);
                pthread_mutex_unlock(&mutex);
                return true;
            }
        } else { // reply->type is not integer or obj has been locked
            freeReplyObject(reply);
            return false;
        }
    } else {
        printf("slow commit misused\n");
        return false;
    }
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
    } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
        strcpy(key_, argv[++i]);
    } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
        strcpy(value_, argv[++i]);
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      s_http_server_opts.document_root = argv[++i];
    } else if (strcmp(argv[i], "--siteid") == 0 && i + 1 < argc) {
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
  printf("redis_cli: PING: %s\n", reply->str);
  freeReplyObject(reply);

  reply = REDIS_COMMAND(redis_cli_master,"PING");
  printf("redis_cli_master: PING: %s\n", reply->str);
  freeReplyObject(reply);

  replicater_init(&replicater, remote_host, remote_port, local_port);
  pthread_create(&(replicater.tid), NULL, replicater.process, &replicater);
  printf("replicater created!\n");

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

  // rep_msg_t msg(SLOW_CMT, key_, value_, strlen(key_), strlen(value_));
  // pthread_mutex_lock(&mutex);
  // msg_list.push_back(msg);
  // pthread_mutex_unlock(&mutex);



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
            "ver1": 0,
            "ver2": 0,
            "user": "bh",
            "content": "今天早上爸爸带我们全家去植物园，沿路上蝉声一直吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱吱……叫个不停，感觉很舒服",
            "utime": 0
          }
        )"_json;


    json d2 =  R"(
          {
            "id": 2,
            "ver1": 0,
            "ver2": 0,
            "user": "bh",
            "content": "今天和妈妈去爬山，到了山顶，妈妈说安静的山间会有回音喔。我和表弟一起试着大叫━━“你好吗”果然大约3秒之后就听到：你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗你好吗（以下删去170多句“你好吗”）……一个字都没变，课本说的音波反射，我终于体会到了。真是有意义的一天。",
            "utime": 0
          }
        )"_json;

    json d3 =  R"(
          {
            "id": 3,
            "ver1": 0,
            "ver2": 0,
            "user": "bh",
            "content": "还记得5天前的我，活蹦乱跳，如猴子一般。而因为显瘦，便连秋裤都没穿，但是现在呢，整天如企鹅一般，因为生病我连最爱的体育课都上不了，还真的是不能逞英雄，原本想逞英雄，如今成狗熊啊。再次跟大家提个醒，多穿点衣服哦，不然就如我一样，英雄变狗熊。",
            "utime": 0
          }
        )"_json;


    reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", "diary_1", d1.dump().c_str());
    freeReplyObject(reply);

    reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", "diary_2", d2.dump().c_str());
    freeReplyObject(reply);

    reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", "diary_3", d3.dump().c_str());
    freeReplyObject(reply);

    std::vector<std::string> users = {"wn", "wsy", "bh", "cs", "xhn", "lzc"};
    std::vector<std::string> comments = {"这是评论1这是评论1这是评论1这是评论1这是评论1", "这是评论2这是评论2这是评论2这是评论2这是评论2",
        "这是评论3这是评论3这是评论3这是评论3", "这是评论4这是评论4这是评论4这是评论4"};

    /// comments
    for (int i = 0; i < 4; ++i) {
        objects::comment com;
        com.id = i;
        com.diary_id = 1;
        com.ver = psi_ver_t(0, 0);
        com.user = users[i];
        com.content = comments[i];
        json j = com;
        reply = REDIS_COMMAND(redis_cli, "RPUSH %s %s", "diary_1_comments", j.dump().c_str());
        freeReplyObject(reply);
    }

    printf("generate_data: done!\n");

}
