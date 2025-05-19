#ifndef SERVER_H
#define SERVER_H


#include <stdbool.h>
#include "common.h"


#define MAX_SALONS 100 
#define MAX_MEMBRES     32
#define MAX_NOM_SALON   50

//Structure Client 
typedef struct {
    char username[50];
    char password[50];
    struct sockaddr_in addr;
    bool connected;
    char salon_courant[MAX_NOM_SALON]; // "" si aucun
} ClientInfo;

//Structure Salon
typedef struct {
    char nom[MAX_NOM_SALON];
    char createur[50];     // pseudo du créateur/admin
    char **membres;        // tableau dynamique de pseudos
    int  nb_membres;       // nombre actuel de membres
    int  membres_capacity; // capacité du tableau membres
} Salon;

//Structure Server
typedef struct {
    int socket_fd;
    struct sockaddr_in server_addr;

    ClientInfo *clients;
    int client_capacity;
    int client_count;
    pthread_mutex_t clients_mutex;

    Salon *salons;
    int nb_salons;
    int salon_capacity;

    pthread_mutex_t salons_mutex;
} Server;

//Fonctions server
int  init_server(Server *server);
void *receive_messages_thread(void *arg);
void  process_request(Server *server, Request *req, struct sockaddr_in *client_addr);
int  send_response(Server *server, Request *res, struct sockaddr_in *client_addr);
int  find_client_by_username(Server *server, const char *username);
int  add_client(Server *server, const char *username, const char *password, 
                struct sockaddr_in *addr);
void remove_client(Server *server, const char *username);

//Fonctions salon
int create_room(Server *server, const char *name, const char *creator);
int delete_room(Server *server, const char *name, const char *username);
int join_room(Server *server, const char *username, const char *room_name);
int add_user(Server *server, const char *user, const char *room);
int remove_user(Server *server, const char *user, const char *room);
void broadcast_room(Server *server, const char *room, Request *msg, const char *sender);
void save_rooms(Server *server, const char *file);
void load_rooms(Server *server, const char *file);

#endif /* SERVER_H */
