// server.h
#ifndef SERVER_H
#define SERVER_H

#include "common.h"
#include <stdbool.h>

#define MAX_CLIENTS 100

// Structure pour stocker les informations des clients connectés
typedef struct {
    char username[50];
    char password[50];
    struct sockaddr_in addr;
    bool connected;
} ClientInfo;

// Structure du serveur
typedef struct {
    int socket_fd;
    struct sockaddr_in server_addr;
    ClientInfo clients[MAX_CLIENTS];
    int client_count;
    pthread_mutex_t clients_mutex;
} Server;

// Structure étendue pour les arguments du thread d'envoi de fichier
typedef struct {
    char filename[256];
    struct sockaddr_in client_addr;
    int success;      // Résultat de l'opération: 1 = succès, 0 = échec
    char message[256]; // Message d'erreur éventuel
} FileTransferArgs;

// Clé pour stocker le pointeur serveur dans les threads
extern pthread_key_t server_key;

// Fonction pour initialiser le serveur
int init_server(Server *server);

// Thread principal pour recevoir des messages
void *receive_messages_thread(void *arg);

// Thread de transfert de fichiers
void *file_transfer_thread(void *arg);

// Fonction pour traiter une requête
void process_request(Server *server, Request *req, struct sockaddr_in *client_addr);

// Fonction pour envoyer une réponse à un client
int send_response(Server *server, Request *res, struct sockaddr_in *client_addr);

// Fonction pour envoyer un fichier à un client
int send_file_to_client(const char *filename, struct sockaddr_in *client_addr);

// Thread pour l'envoi de fichier
void *file_send_thread_func(void *arg);

// Fonction pour trouver un client par son nom d'utilisateur
int find_client_by_username(Server *server, const char *username);

// Fonction pour ajouter un client
int add_client(Server *server, const char *username, const char *password, 
               struct sockaddr_in *addr);

// Fonction pour supprimer un client
void remove_client(Server *server, const char *username);

#endif /* SERVER_H */