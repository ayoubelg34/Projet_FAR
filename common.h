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

#define MAX_MSG_SIZE 1024
#define SERVER_PORT 8888

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

#endif /* COMMON_H */