// client.h
#ifndef CLIENT_H
#define CLIENT_H

#include "common.h"

// Structure pour stocker les informations du client
typedef struct {
    int socket_fd;
    struct sockaddr_in server_addr;
    char username[50];
    char password[50];
} Client;

// Fonction pour initialiser le client
int init_client(Client *client, const char *server_ip);

// Fonction pour se connecter au serveur
int connect_to_server(Client *client, const char *username, const char *password);

// Thread pour l'envoi de messages
void *send_message_thread(void *arg);

// Thread pour la réception de messages
void *receive_message_thread(void *arg);

// Fonction pour envoyer une requête
int send_request(Client *client, Request *req);

#endif /* CLIENT_H */   