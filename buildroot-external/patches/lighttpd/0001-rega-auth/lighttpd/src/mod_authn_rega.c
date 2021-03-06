/*
 * mod_authn_rega.c
 *
 *  Copyright (C) 2018 eQ-3 Entwicklung GmbH
 *  Author: Christian Niclaus
 */

#include "base.h"
#include "plugin.h"
#include "http_auth.h"
#include "log.h"
#include "response.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct {
  int rega_port;
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config **config_storage;
    plugin_config conf;
} plugin_data;

int checkAuth(int port, const char* user, const char* pass);
void sendMsg(const char* msg, const int msgLength, const int regaPort, char* answer, int* answerLength);

#define PATCH(x) \
    p->conf.x = s->x;

static int mod_authn_rega_patch_connection(server *srv, connection *con, plugin_data *p) {
    size_t i, j;
    plugin_config *s = p->config_storage[0];

    PATCH(rega_port);

    /* skip the first, the global context */
    for (i = 1; i < srv->config_context->used; i++) {
        data_config *dc = (data_config *)srv->config_context->data[i];
        s = p->config_storage[i];

        /* condition didn't match */
        if (!config_check_cond(srv, con, dc)) continue;

        /* merge config */
        for (j = 0; j < dc->value->used; j++) {
            data_unset *du = dc->value->data[j];

            if (buffer_is_equal_string(du->key, CONST_STR_LEN("auth.backend.rega.port"))) {
                PATCH(rega_port);
            }
        }
    }

    return 0;
}
#undef PATCH

static handler_t mod_authn_rega_basic(server *srv, connection *con, void *p_d, const http_auth_require_t *require, const buffer *username, const char *pw) {
    plugin_data *p = (plugin_data *)p_d;
//    int rc;

   mod_authn_rega_patch_connection(srv, con, p);

   if(1 == checkAuth(p->conf.rega_port, username->ptr, pw)) {
     //auth ok
     return HANDLER_GO_ON;
   }
   else {
     return HANDLER_ERROR;
   }

}

int checkAuth(int port, const char* user, const char* pass) {

  //assemble message

  char msg[1024];
  memset(msg, 0, sizeof(char)*1024);

  const int lengthUser = strlen(user);
  const int lengthPass = strlen(pass);
  if((lengthUser + lengthPass + 1)  > 1023) {
    return 0;
  }

  int m = 0;
  //user
  for(int i = 0 ; i < lengthUser && m <= 1022; i++) {
    const char c = user[i];
    if(c == '\\') {
      msg[m] = '\\';
      m++;
      msg[m] = '\\';
    }
    else if(c == ':') {
      msg[m] = '\\';
      m++;
      msg[m] = ':';
    }
    else {
      msg[m] = c;
    }
    m++;
  }
  //delimiter
  msg[m] = ':';
  m++;
  //pass
  for(int i = 0; i < lengthPass && m <= 1022; i++) {
    const char c = pass[i];
    if(c == '\\') {
      msg[m] = '\\';
      m++;
      msg[m] = '\\';
    }
    else if(c == ':') {
      msg[m] = '\\';
      m++;
      msg[m] = ':';
    }
    else {
      msg[m] = c;
    }
    m++;
  }


  //Create socket and send
  int answerLength = 128;
  char answer[answerLength];
  memset(answer, 0 ,sizeof(char)*answerLength);
  sendMsg(msg, m, port, answer, &answerLength);
  if(strcmp("1", answer) == 0) {
    return 1;
  }

  return 0;
}

void sendMsg(const char* msg, const int msgLength, const int regaPort, char* answer, int* answerLength) {
  const int answerBufferLength = *answerLength;
  *answerLength = 0;

  //create socket
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if(sock == -1) {
    return;
  }

  struct sockaddr_in addrRega, addrMe;

  memset(&addrRega, 0, sizeof(addrRega));
  addrRega.sin_family = AF_INET;
  addrRega.sin_addr.s_addr = inet_addr("127.0.0.1");
  addrRega.sin_port = htons(regaPort);
  memset(&addrMe, 0, sizeof(addrMe));
  addrMe.sin_family = AF_INET;
  addrMe.sin_addr.s_addr = inet_addr("127.0.0.1");
  //addrMe.sin_port = htons(0);

  if(bind(sock, &addrMe, sizeof(addrMe)) == -1) {
    return;
  }

  //send msg
  if(sendto(sock, msg, msgLength, 0, &addrRega, sizeof(addrRega)) == -1 ) {
    return;
  }

  //receive answer
  memset(&addrRega, 0, sizeof(addrRega));
  unsigned int sLength = 0;
  *answerLength = recvfrom(sock, answer, answerBufferLength, 0, &addrRega, &sLength);
  //check if the answer comes from correct port....
  if(sLength != 0) {
    char* name = inet_ntoa(addrRega.sin_addr);
    int port = ntohs(addrRega.sin_port);

    if( ! (port == regaPort && (strcmp(name, "127.0.0.1") == 0))) {
       *answerLength = 0;
    }
  }

  close(sock);
}

/*static handler_t mod_authn_rega_plain_digest(server *srv, connection *con, void *p_d, const char *username, const char *realm, unsigned char HA1[16]) {
  return HANDLER_ERROR;
}*/

INIT_FUNC(mod_authn_rega_init) {
    static http_auth_backend_t http_auth_backend_rega =
      { "rega", mod_authn_rega_basic, NULL/*mod_authn_rega_digest*/, NULL };


    plugin_data *p = calloc(1, sizeof(*p));

    /* register http_auth_backend_rega */
    http_auth_backend_rega.p_d = p;

    http_auth_backend_set(&http_auth_backend_rega);

    return p;
}


FREE_FUNC(mod_authn_rega_free) {
    plugin_data *p = p_d;

    UNUSED(srv);

    if (!p) return HANDLER_GO_ON;

    if (p->config_storage) {
        size_t i;
        for (i = 0; i < srv->config_context->used; i++) {
            plugin_config *s = p->config_storage[i];

            if (NULL == s) continue;

            //buffer_free(s->auth_plain_groupfile);
            //buffer_free(s->auth_plain_userfile);
            //buffer_free(s->auth_htdigest_userfile);
            //buffer_free(s->auth_htpasswd_userfile);

            free(s);
        }
        free(p->config_storage);
    }

    free(p);

    return HANDLER_GO_ON;
}

SETDEFAULTS_FUNC(mod_authn_rega_set_defaults) {
    plugin_data *p = p_d;
    size_t i;

    config_values_t cv[] = {
        { "auth.backend.rega.port",   NULL, T_CONFIG_INT, T_CONFIG_SCOPE_CONNECTION }, /* 0 */
        { NULL,                       NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
    };

    p->config_storage = calloc(1, srv->config_context->used * sizeof(plugin_config *));

    for (i = 0; i < srv->config_context->used; i++) {
        data_config const* config = (data_config const*)srv->config_context->data[i];
        plugin_config *s;

        s = calloc(1, sizeof(plugin_config));

        //s->auth_plain_groupfile = buffer_init();
        //s->auth_plain_userfile = buffer_init();
        //s->auth_htdigest_userfile = buffer_init();
        //s->auth_htpasswd_userfile = buffer_init();

        cv[0].destination = &s->rega_port;
    //    cv[1].destination = s->auth_plain_userfile;
      //  cv[2].destination = s->auth_htdigest_userfile;
       // cv[3].destination = s->auth_htpasswd_userfile;

        p->config_storage[i] = s;

        if (0 != config_insert_values_global(srv, config->value, cv, i == 0 ? T_CONFIG_SCOPE_SERVER : T_CONFIG_SCOPE_CONNECTION)) {
            return HANDLER_ERROR;
        }
    }

    return HANDLER_GO_ON;
}


int mod_authn_rega_plugin_init(plugin *p);
int mod_authn_rega_plugin_init(plugin *p) {
    p->version     = LIGHTTPD_VERSION_ID;
    p->name        = buffer_init_string("authn_rega");
    p->init        = mod_authn_rega_init;
    p->set_defaults= mod_authn_rega_set_defaults;
    p->cleanup     = mod_authn_rega_free;

    p->data        = NULL;

    return 0;
}
