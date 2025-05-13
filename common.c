// common.c
#include "common.h"

volatile sig_atomic_t running = 1;
int global_socket_fd = -1;  // Initialisation de la variable globale

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
    printf("\nInterruption reçue (signal %d). Arrêt en cours...\n", sig);
    // Just set the running flag to 0, don't close the socket here
    // This will allow the main thread to send shutdown messages before closing
    running = 0;
}