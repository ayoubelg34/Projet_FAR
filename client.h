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

// Structure pour les arguments du thread de transfert de fichier
typedef struct {
    char filename[256];
    char server_ip[16];
    int port;
    char save_dir[256];
    int is_upload;  // 1 = upload, 0 = download
} FileTransferThreadArgs;

// Fonction pour initialiser le client
int init_client(Client *client, const char *server_ip);

// Fonction pour se connecter au serveur
int connect_to_server(Client *client, const char *username, const char *password);

// Thread pour l'envoi de messages
void *send_message_thread(void *arg);

// Thread pour la réception de messages
void *receive_message_thread(void *arg);

// Thread pour le transfert de fichier en arrière-plan
void *file_transfer_thread(void *arg);

// Fonction pour envoyer une requête
int send_request(Client *client, Request *req);

// Fonction pour envoyer un fichier via TCP
int send_file(const char *filename, const char *server_ip);

// Fonction pour recevoir un fichier via TCP
int receive_file(const char *save_dir, const char *server_ip);

// Fonction pour recevoir un fichier via TCP avec un port spécifié
int receive_file_with_port(const char *save_dir, const char *server_ip, int port);

// Clé pour stocker le pointeur client dans les threads
extern pthread_key_t client_key;

#endif /* CLIENT_H */