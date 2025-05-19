// command.c
#include "command.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <libgen.h>
#include <dirent.h>

// Tableau des commandes disponibles
static Command commands[] = {
    {"help", cmd_help, "Affiche la liste des commandes disponibles", ROLE_USER},
    {"ping", cmd_ping, "Test de connectivité avec le serveur", ROLE_USER},
    {"msg", cmd_msg, "Envoie un message privé (@msg <user> <message>)", ROLE_USER},
    {"credits", cmd_credits, "Affiche les crédits de l'application", ROLE_USER},
    {"shutdown", cmd_shutdown, "Arrête le serveur (admin uniquement)", ROLE_ADMIN},
    {"list", cmd_list, "Affiche la liste des utilisateurs connectés", ROLE_USER},
    {"download", cmd_download, "Télécharge un des fichiers du serveur pour le client", ROLE_USER},
    {"upload", cmd_upload, "Envoie un fichier sur le serveur", ROLE_USER},
    {"promote", cmd_promote, "Promeut un utilisateur au rang de modérateur (admin uniquement)", ROLE_ADMIN},
    {"disconnect", cmd_disconnect, "Déconnecte explicitement du serveur", ROLE_USER},
    {"uploads", cmd_uploads, "Affiche la liste des fichiers disponibles sur le serveur", ROLE_USER},
    {"mute", cmd_mute, "Rend muet un utilisateur pendant une durée spécifiée (@mute <user> <minutes>)", ROLE_MODERATOR},
    {"unmute", cmd_unmute, "Annule le mode muet d'un utilisateur (@unmute <user>)", ROLE_MODERATOR}
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
    
    // Trouver l'utilisateur
    int client_idx = find_client_by_username(server, req->sender);
    if (client_idx < 0) {
        // Utilisateur non trouvé
        Request response;
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Erreur: Vous n'êtes pas connecté au serveur.");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Obtenir le rôle de l'utilisateur
    UserRole user_role = server->clients[client_idx].role;
    
    // Chercher la commande dans le tableau
    for (int i = 0; i < command_count; i++) {
        if (strcmp(cmd_name, commands[i].name) == 0) {
            // Vérifier les droits d'accès
            if (user_role < commands[i].min_role) {
                // Droits insuffisants
                Request response;
                init_request(&response, REQ_MESSAGE, "Server", "", 
                             "Erreur: Vous n'avez pas les droits suffisants pour exécuter cette commande.");
                send_response(server, &response, client_addr);
                return CMD_ERROR;
            }
            
            // Exécuter la commande
            return commands[i].handler(server, req, client_addr);
        }
    }
    
    // Commande inconnue
    Request response;
    init_request(&response, REQ_MESSAGE, "Server", "", 
                 "Commande inconnue. Tapez @help pour voir les commandes disponibles.");
    send_response(server, &response, client_addr);
    
    return CMD_ERROR;
}

CommandResult cmd_help(Server *server, Request *req, struct sockaddr_in *client_addr) {
    (void)req;  // Mark parameter as intentionally unused to fix warning
    
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
            // Use snprintf to avoid buffer overflow
            snprintf(line, sizeof(line), "@%s - %s\n", commands[i].name, commands[i].description);
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
    (void)req;  // Mark parameter as intentionally unused to fix warning
    
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
        snprintf(error, sizeof(error), "Utilisateur '%s' non trouvé", recipient);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Envoyer le message privé
    char private_msg[MAX_MSG_SIZE];
    // Use snprintf to prevent buffer overflow
    snprintf(private_msg, sizeof(private_msg), "[Message privé de %s]: %s", req->sender, message);
    init_request(&response, REQ_MESSAGE, "Server", "", private_msg);
    send_response(server, &response, &server->clients[recipient_idx].addr);
    
    // Confirmer l'envoi à l'expéditeur
    snprintf(private_msg, sizeof(private_msg), "Message privé envoyé à %s", recipient);
    init_request(&response, REQ_MESSAGE, "Server", "", private_msg);
    send_response(server, &response, client_addr);
    
    pthread_mutex_unlock(&server->clients_mutex);
    return CMD_SUCCESS;
}

CommandResult cmd_credits(Server *server, Request *req, struct sockaddr_in *client_addr) {
    (void)req;  // Mark parameter as intentionally unused to fix warning
    
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

CommandResult cmd_list(Server *server, Request *req, struct sockaddr_in *client_addr) {
    (void)req;  // Mark parameter as intentionally unused to fix warning
    
    Request response;
    char message[MAX_MSG_SIZE];
    strcpy(message, "Utilisateurs connectés:\n");
    
    pthread_mutex_lock(&server->clients_mutex);
    
    int connected_count = 0;
    for (int i = 0; i < server->client_count; i++) {
        if (server->clients[i].connected) {
            char line[64];
            sprintf(line, "- %s\n", server->clients[i].username);
            
            if (strlen(message) + strlen(line) < MAX_MSG_SIZE - 1) {
                strcat(message, line);
                connected_count++;
            }
        }
    }
    
    if (connected_count == 0) {
        strcpy(message, "Aucun utilisateur connecté");
    } else {
        char count_line[64];
        sprintf(count_line, "\nTotal: %d utilisateur(s) connecté(s)", connected_count);
        if (strlen(message) + strlen(count_line) < MAX_MSG_SIZE - 1) {
            strcat(message, count_line);
        }
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
    
    init_request(&response, REQ_MESSAGE, "Server", "", message);
    send_response(server, &response, client_addr);
    return CMD_SUCCESS;
}

CommandResult cmd_download(Server *server, Request *req, struct sockaddr_in *client_addr) {
    (void)req;  // Mark parameter as intentionally unused to fix warning
    
    Request response;
    char *args = get_command_args(req->content);
    
    // Check if args is empty
    if (args[0] == '\0') {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Usage: @download <filename> - Télécharge un fichier depuis le serveur");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Extract the filename
    char filename[256];
    sscanf(args, "%255s", filename);
    
    // Full path to file (in uploads directory as that's what server can share)
    char filepath[512];
    sprintf(filepath, "./uploads/%s", filename);
    
    // Log what file we're trying to open
    printf("Attempting to open file: %s\n", filepath);
    
    // Check if file exists
    FILE *file = fopen(filepath, "rb");  // Use binary mode for images and other binary files
    if (!file) {
        char error_msg[256];
        sprintf(error_msg, "Erreur: Fichier '%s' introuvable sur le serveur", filename);
        init_request(&response, REQ_MESSAGE, "Server", "", error_msg);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    fclose(file);
    
    // Notify client that file transfer will start
    char notify_msg[256];
    sprintf(notify_msg, "Début du téléchargement de '%s'...", filename);
    init_request(&response, REQ_MESSAGE, "Server", "", notify_msg);
    send_response(server, &response, client_addr);
    
    // Start file transfer thread
    pthread_t file_thread;
    FileTransferArgs *args_struct = malloc(sizeof(FileTransferArgs));
    if (!args_struct) {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Erreur: Impossible d'allouer la mémoire pour le transfert");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Configure transfer args
    strncpy(args_struct->filename, filename, sizeof(args_struct->filename) - 1);
    memcpy(&args_struct->client_addr, client_addr, sizeof(struct sockaddr_in));
    
    // Create thread for file transfer
    if (pthread_create(&file_thread, NULL, file_send_thread_func, args_struct) != 0) {
        free(args_struct);
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Erreur: Impossible de démarrer le thread de transfert");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Detach thread to avoid memory leaks
    pthread_detach(file_thread);
    
    return CMD_SUCCESS;
}

CommandResult cmd_upload(Server *server, Request *req, struct sockaddr_in *client_addr) {
    (void)req;  // Mark parameter as intentionally unused to fix warning
    
    Request response;
    char *args = get_command_args(req->content);
    
    // Check if args is empty
    if (args[0] == '\0') {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Usage: @upload <filename> - Envoie un fichier au serveur");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Extract the filename
    char filename[256];
    sscanf(args, "%255s", filename);
    
    // Get just the base filename without path
    char *base_filename = basename(filename);
    
    // Notify client that we're ready to receive
    char notify_msg[256];
    sprintf(notify_msg, "Serveur prêt à recevoir le fichier '%s'. Envoi en cours...", base_filename);
    init_request(&response, REQ_MESSAGE, "Server", "", notify_msg);
    send_response(server, &response, client_addr);
    
    // Make sure uploads directory exists
    struct stat st = {0};
    if (stat("./uploads", &st) == -1) {
        mkdir("./uploads", 0700);
    }
    
    // Generate destination path
    char dest_path[512];
    char unique_filename[256];
    generate_unique_filename("./uploads", base_filename, unique_filename, sizeof(unique_filename));
    sprintf(dest_path, "./uploads/%s", unique_filename);
    
    // Get client IP
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    
    // Create thread for file transfer
    pthread_t file_thread;
    FileTransferArgs *args_struct = malloc(sizeof(FileTransferArgs));
    if (!args_struct) {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Erreur: Impossible d'allouer la mémoire pour le transfert");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Configure transfer args
    strncpy(args_struct->filename, unique_filename, sizeof(args_struct->filename) - 1);
    memcpy(&args_struct->client_addr, client_addr, sizeof(struct sockaddr_in));
    args_struct->success = 0;
    
    // Start file transfer thread
    if (pthread_create(&file_thread, NULL, file_transfer_thread, args_struct) != 0) {
        free(args_struct);
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Erreur: Impossible de démarrer le thread de réception");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Detach thread to avoid memory leaks
    pthread_detach(file_thread);
    
    return CMD_SUCCESS;
}

CommandResult cmd_promote(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    char *args = get_command_args(req->content);
    
    // Vérifier les arguments
    if (args[0] == '\0') {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Usage: @promote <utilisateur> - Promouvoir un utilisateur au rang de modérateur");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Extraire le nom d'utilisateur
    char username[50];
    sscanf(args, "%49s", username);
    
    // Trouver l'utilisateur à promouvoir
    pthread_mutex_lock(&server->clients_mutex);
    int user_idx = find_client_by_username(server, username);
    
    if (user_idx < 0) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        snprintf(error, sizeof(error), "Utilisateur '%s' non trouvé", username);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Vérifier s'il est déjà modérateur ou admin
    if (server->clients[user_idx].role >= ROLE_MODERATOR) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        snprintf(error, sizeof(error), "L'utilisateur '%s' est déjà modérateur ou administrateur", username);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Promouvoir l'utilisateur
    server->clients[user_idx].role = ROLE_MODERATOR;
    pthread_mutex_unlock(&server->clients_mutex);
    
    // Envoyer confirmation
    char confirm[128];
    snprintf(confirm, sizeof(confirm), "L'utilisateur '%s' a été promu au rang de modérateur", username);
    init_request(&response, REQ_MESSAGE, "Server", "", confirm);
    send_response(server, &response, client_addr);
    
    // Notifier l'utilisateur promu
    char notify[128];
    snprintf(notify, sizeof(notify), "Vous avez été promu au rang de modérateur par '%s'", req->sender);
    init_request(&response, REQ_MESSAGE, "Server", "", notify);
    send_response(server, &response, &server->clients[user_idx].addr);
    
    return CMD_SUCCESS;
}

CommandResult cmd_disconnect(Server *server, Request *req, struct sockaddr_in *client_addr) {
    // Trouver l'utilisateur
    int client_idx = find_client_by_username(server, req->sender);
    if (client_idx < 0) {
        // Étrange, l'utilisateur n'est pas trouvé
        Request response;
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Erreur interne: utilisateur non trouvé");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Envoyer confirmation à l'utilisateur
    Request response;
    init_request(&response, REQ_MESSAGE, "Server", "", 
                 "Déconnexion en cours. Au revoir!");
    send_response(server, &response, client_addr);
    
    // Marquer l'utilisateur comme déconnecté
    pthread_mutex_lock(&server->clients_mutex);
    server->clients[client_idx].connected = false;
    pthread_mutex_unlock(&server->clients_mutex);
    
    // Annoncer la déconnexion aux autres clients
    char announce[100];
    sprintf(announce, "%s a quitté le chat", req->sender);
    init_request(&response, REQ_MESSAGE, "Server", "", announce);
    
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < server->client_count; i++) {
        if (i != client_idx && server->clients[i].connected) {
            send_response(server, &response, &server->clients[i].addr);
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
    
    return CMD_SUCCESS;
}

CommandResult cmd_uploads(Server *server, Request *req, struct sockaddr_in *client_addr) {
    (void)req;  // Pour éviter l'avertissement de paramètre non utilisé
    
    Request response;
    char message[MAX_MSG_SIZE] = "Fichiers disponibles sur le serveur:\n";
    
    // Ouvrir le répertoire
    DIR *dir = opendir("./uploads");
    if (!dir) {
        // Si le répertoire n'existe pas, le créer et envoyer message vide
        mkdir("./uploads", 0755);
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Aucun fichier disponible sur le serveur");
        send_response(server, &response, client_addr);
        return CMD_SUCCESS;
    }
    
    // Lire les entrées du répertoire
    struct dirent *entry;
    int file_count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        // Ignorer "." et ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Ajouter le nom du fichier à la liste
        char line[256];
        snprintf(line, sizeof(line), "- %s\n", entry->d_name);
        
        // Vérifier si l'ajout dépasserait la taille maximale
        if (strlen(message) + strlen(line) < MAX_MSG_SIZE - 64) {
            strcat(message, line);
            file_count++;
        } else {
            // Trop de fichiers, ajouter message de troncation
            strcat(message, "...(liste tronquée)\n");
            break;
        }
    }
    
    closedir(dir);
    
    // Si aucun fichier trouvé
    if (file_count == 0) {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Aucun fichier disponible sur le serveur");
    } else {
        // Ajouter récapitulatif
        char summary[64];
        snprintf(summary, sizeof(summary), "\nTotal: %d fichier(s)\n", file_count);
        strcat(message, summary);
        
        // Ajouter instruction pour téléchargement
        strcat(message, "Pour télécharger un fichier: @download <nom_fichier>");
        
        init_request(&response, REQ_MESSAGE, "Server", "", message);
    }
    
    send_response(server, &response, client_addr);
    return CMD_SUCCESS;
}

CommandResult cmd_mute(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    char *args = get_command_args(req->content);
    
    // Vérifier les arguments
    char username[50];
    int minutes = 10; // Durée par défaut: 10 minutes
    
    if (sscanf(args, "%49s %d", username, &minutes) < 1) {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Usage: @mute <utilisateur> [minutes] - Rend muet un utilisateur (10 minutes par défaut)");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Limiter la durée maximale (60 minutes = 1 heure)
    if (minutes > 60) {
        minutes = 60;
    } else if (minutes < 1) {
        minutes = 1; // Minimum 1 minute
    }
    
    // Trouver l'utilisateur à rendre muet
    pthread_mutex_lock(&server->clients_mutex);
    int user_idx = find_client_by_username(server, username);
    
    if (user_idx < 0) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        snprintf(error, sizeof(error), "Utilisateur '%s' non trouvé", username);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Ne pas permettre de rendre muet un administrateur ou un modérateur
    if (server->clients[user_idx].role >= ROLE_MODERATOR) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        snprintf(error, sizeof(error), "Vous ne pouvez pas rendre muet un modérateur ou un administrateur");
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Rendre l'utilisateur muet
    server->clients[user_idx].is_muted = true;
    server->clients[user_idx].mute_until = time(NULL) + (minutes * 60);
    
    pthread_mutex_unlock(&server->clients_mutex);
    
    // Envoyer confirmation
    char confirm[128];
    snprintf(confirm, sizeof(confirm), "L'utilisateur '%s' a été rendu muet pendant %d minutes", username, minutes);
    init_request(&response, REQ_MESSAGE, "Server", "", confirm);
    send_response(server, &response, client_addr);
    
    // Notifier l'utilisateur concerné
    char notify[128];
    snprintf(notify, sizeof(notify), "Vous avez été rendu muet par '%s' pendant %d minutes", req->sender, minutes);
    init_request(&response, REQ_MESSAGE, "Server", "", notify);
    send_response(server, &response, &server->clients[user_idx].addr);
    
    return CMD_SUCCESS;
}

CommandResult cmd_unmute(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    char *args = get_command_args(req->content);
    
    // Vérifier les arguments
    char username[50];
    
    if (sscanf(args, "%49s", username) != 1) {
        init_request(&response, REQ_MESSAGE, "Server", "", 
                     "Usage: @unmute <utilisateur> - Annule le mode muet d'un utilisateur");
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Trouver l'utilisateur
    pthread_mutex_lock(&server->clients_mutex);
    int user_idx = find_client_by_username(server, username);
    
    if (user_idx < 0) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        snprintf(error, sizeof(error), "Utilisateur '%s' non trouvé", username);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Vérifier si l'utilisateur est actuellement muet
    if (!server->clients[user_idx].is_muted) {
        pthread_mutex_unlock(&server->clients_mutex);
        char error[128];
        snprintf(error, sizeof(error), "L'utilisateur '%s' n'est pas muet", username);
        init_request(&response, REQ_MESSAGE, "Server", "", error);
        send_response(server, &response, client_addr);
        return CMD_ERROR;
    }
    
    // Annuler le mode muet
    server->clients[user_idx].is_muted = false;
    server->clients[user_idx].mute_until = 0;
    
    pthread_mutex_unlock(&server->clients_mutex);
    
    // Envoyer confirmation
    char confirm[128];
    snprintf(confirm, sizeof(confirm), "Le mode muet de l'utilisateur '%s' a été annulé", username);
    init_request(&response, REQ_MESSAGE, "Server", "", confirm);
    send_response(server, &response, client_addr);
    
    // Notifier l'utilisateur concerné
    char notify[128];
    snprintf(notify, sizeof(notify), "Votre mode muet a été annulé par '%s'", req->sender);
    init_request(&response, REQ_MESSAGE, "Server", "", notify);
    send_response(server, &response, &server->clients[user_idx].addr);
    
    return CMD_SUCCESS;
}
