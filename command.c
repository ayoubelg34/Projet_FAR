// command.c
#include "command.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Tableau des commandes disponibles
static Command commands[] = {
    {"help", cmd_help, "Affiche la liste des commandes disponibles"},
    {"ping", cmd_ping, "Test de connectivité avec le serveur"},
    {"msg", cmd_msg, "Envoie un message privé (@msg <user> <message>)"},
    {"credits", cmd_credits, "Affiche les crédits de l'application"},
    {"shutdown", cmd_shutdown, "Arrête le serveur (admin uniquement)"}
};

static int command_count = sizeof(commands) / sizeof(Command);

void init_command_system(void) {
    printf("Système de commandes initialisé avec %d commandes\n", command_count);
}

char* get_command_name(const char *command_str) {
    static char name[32];
    
    // Ignorer le '@' initial
    const char *start = (command_str[0] == '@') ? command_str + 1 : command_str;
    
    // Copier jusqu'au premier espace
    int i = 0;
    while (start[i] && start[i] != ' ' && i < 31) {
        name[i] = start[i];
        i++;
    }
    name[i] = '\0';
    
    return name;
}

char* get_command_args(const char *command_str) {
    static char args[MAX_MSG_SIZE];
    
    // Trouver le premier espace
    const char *space = strchr(command_str, ' ');
    if (!space) {
        args[0] = '\0';
        return args;
    }
    
    // Copier tout après le premier espace
    strncpy(args, space + 1, MAX_MSG_SIZE - 1);
    args[MAX_MSG_SIZE - 1] = '\0';
    
    return args;
}

char* read_file_content(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        char *error_msg = malloc(128);
        sprintf(error_msg, "Erreur: impossible d'ouvrir le fichier %s", filename);
        return error_msg;
    }
    
    // Obtenir la taille du fichier
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    
    // Allouer la mémoire
    char *content = malloc(size + 1);
    if (!content) {
        fclose(file);
        char *error_msg = malloc(64);
        sprintf(error_msg, "Erreur: allocation mémoire insuffisante");
        return error_msg;
    }
    
    // Lire le fichier
    fread(content, 1, size, file);
    content[size] = '\0';
    
    fclose(file);
    return content;
}

CommandResult process_command(Server *server, Request *req, struct sockaddr_in *client_addr) {
    char *cmd_name = get_command_name(req->content);
    
    // Chercher la commande dans le tableau
    for (int i = 0; i < command_count; i++) {
        if (strcmp(cmd_name, commands[i].name) == 0) {
            return commands[i].handler(server, req, client_addr);
        }
    }
    
    // Commande inconnue
    Request response;
    init_request(&response, REQ_MESSAGE, "Server", "", "Commande inconnue. Tapez @help pour voir les commandes disponibles.");
    send_response(server, &response, client_addr);
    
    return CMD_ERROR;
}

CommandResult cmd_help(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    
    // Essayer de lire le fichier help.txt
    char *content = read_file_content("help.txt");
    
    // Si le fichier n'existe pas, afficher la liste des commandes
    if (strstr(content, "Erreur") != NULL) {
        free(content);
        
        char help_msg[MAX_MSG_SIZE];
        strcpy(help_msg, "Commandes disponibles:\n");
        
        for (int i = 0; i < command_count; i++) {
            char line[128];
            sprintf(line, "@%s - %s\n", commands[i].name, commands[i].description);
            if (strlen(help_msg) + strlen(line) < MAX_MSG_SIZE - 1) {
                strcat(help_msg, line);
            }
        }
        
        init_request(&response, REQ_MESSAGE, "Server", "", help_msg);
    } else {
        init_request(&response, REQ_MESSAGE, "Server", "", content);
        free(content);
    }
    
    send_response(server, &response, client_addr);
    return CMD_SUCCESS;
}

CommandResult cmd_ping(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    init_request(&response, REQ_MESSAGE, "Server", "", "pong");
    send_response(server, &response, client_addr);
    return CMD_SUCCESS;
}

CommandResult cmd_msg(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    char *args = get_command_args(req->content);
    
    // Parser les arguments : @msg <user> <message>
    char recipient[50];
    char message[MAX_MSG_SIZE];
    
    if (sscanf(args, "%49s %[^\n]", recipient, message) != 2) {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Usage: @msg <utilisateur> <message>");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Trouver le destinataire
    pthread_mutex_lock(&server->clients_mutex);
    int recipient_idx = find_client_by_username(server, recipient);
    
    if (recipient_idx < 0) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        sprintf(error, "Utilisateur '%s' non trouvé", recipient);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Envoyer le message privé
    char private_msg[MAX_MSG_SIZE];
    sprintf(private_msg, "[Message privé de %s]: %s", req->sender, message);
    init_request(&response, REQ_MESSAGE, "Server", "", private_msg);
    send_response(server, &response, &server->clients[recipient_idx].addr);
    
    // Confirmer l'envoi à l'expéditeur
    sprintf(private_msg, "Message privé envoyé à %s", recipient);
    init_request(&response, REQ_MESSAGE, "Server", "", private_msg);
    send_response(server, &response, client_addr);
    
    pthread_mutex_unlock(&server->clients_mutex);
    return CMD_SUCCESS;
}

CommandResult cmd_credits(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    
    // Lire le fichier credits.txt
    char *content = read_file_content("credits.txt");
    
    // Si le fichier n'existe pas, afficher des crédits par défaut
    if (strstr(content, "Erreur") != NULL) {
        free(content);
        content = malloc(256);
        strcpy(content, "Application de messagerie\nDéveloppée dans le cadre du projet FAR\nÉquipe : [Vos noms ici]");
    }
    
    init_request(&response, REQ_MESSAGE, "Server", "", content);
    send_response(server, &response, client_addr);
    
    free(content);
    return CMD_SUCCESS;
}

CommandResult cmd_shutdown(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    
    // TODO: Ajouter vérification des droits admin
    printf("Commande shutdown reçue de %s\n", req->sender);
    
    init_request(&response, REQ_MESSAGE, "Server", "", "Arrêt du serveur en cours...");
    send_response(server, &response, client_addr);
    
    // Déclencher l'arrêt du serveur
    running = 0;
    
    return CMD_SHUTDOWN;
}