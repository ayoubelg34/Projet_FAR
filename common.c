// common.c
#include "common.h"

volatile sig_atomic_t running = 1;

void init_request(Request *req, RequestType type, const char *sender, 
                  const char *recipient, const char *content) {
    req->type = type;
    
    strncpy(req->sender, sender, sizeof(req->sender) - 1);
    req->sender[sizeof(req->sender) - 1] = '\0';
    
    strncpy(req->recipient, recipient, sizeof(req->recipient) - 1);
    req->recipient[sizeof(req->recipient) - 1] = '\0';
    
    strncpy(req->content, content, sizeof(req->content) - 1);
    req->content[sizeof(req->content) - 1] = '\0';
}

void handle_sigint(int sig) {
    running = 0;
    printf("\nArrêt en cours...\n");
}