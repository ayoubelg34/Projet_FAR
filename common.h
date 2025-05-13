// Ajouter dans common.h
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>  // Ajouté pour les codes d'erreur comme EAGAIN
#include <libgen.h>  // For basename()
#include <sys/stat.h> // For mkdir()
#include <sys/time.h> // to use timeval structs

#define MAX_MSG_SIZE 1024
#define SERVER_PORT 8888
#define FILE_TRANSFER_PORT 9876

// Ajouter ces déclarations
extern int global_socket_fd;  // Socket globale pour la gestion du signal
extern volatile sig_atomic_t running;

// Types de requêtes
typedef enum {
    REQ_MESSAGE,       // Message standard
    REQ_COMMAND,       // Commande (préfixée par @)
    REQ_CONNECT,       // Connexion d'un utilisateur
    REQ_DISCONNECT     // Déconnexion
} RequestType;

// Structure de requête
typedef struct {
    RequestType type;
    char sender[50];           // Expéditeur
    char recipient[50];        // Destinataire (pour messages privés)
    char content[MAX_MSG_SIZE]; // Contenu du message/commande
} Request;

// Fonction pour initialiser une requête
void init_request(Request *req, RequestType type, const char *sender, 
                  const char *recipient, const char *content);

// Gestionnaire de signal pour SIGINT
void handle_sigint(int sig);

// Fonction pour générer un nom de fichier unique s'il existe déjà
char* generate_unique_filename(const char *dir, const char *original_filename, char *buffer, size_t buffer_size);

// Fonction pour envoyer un fichier via TCP
// Mode: 0 = client envoi au serveur, 1 = serveur envoi au client
int send_file_tcp(const char *filename, const char *storage_path, const char *remote_ip, int port, int mode);

#endif /* COMMON_H */