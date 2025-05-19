// command.h
#ifndef COMMAND_H
#define COMMAND_H

#include "common.h"
#include "server.h"

// Type de retour des commandes
typedef enum {
    CMD_SUCCESS,
    CMD_ERROR,
    CMD_SHUTDOWN
} CommandResult;

// Structure pour une commande
typedef struct {
    char name[32];
    CommandResult (*handler)(Server *server, Request *req, struct sockaddr_in *client_addr);
    char description[128];
    UserRole min_role; // Rôle minimum requis pour exécuter la commande
} Command;

// Initialisation du système de commandes
void init_command_system(void);

// Traitement d'une commande
CommandResult process_command(Server *server, Request *req, struct sockaddr_in *client_addr);

// Commandes spécifiques
CommandResult cmd_help(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_ping(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_msg(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_credits(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_shutdown(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_list(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_download(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_upload(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_promote(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_disconnect(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_uploads(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_mute(Server *server, Request *req, struct sockaddr_in *client_addr);
CommandResult cmd_unmute(Server *server, Request *req, struct sockaddr_in *client_addr);

// Utilitaires
char* read_file_content(const char *filename);
char* get_command_name(const char *command_str);
char* get_command_args(const char *command_str);

#endif /* COMMAND_H */