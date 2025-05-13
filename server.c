// server.c
#include "server.h"
#include "command.h"

int init_server(Server *server) {
    // Créer la socket UDP
    server->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->socket_fd < 0) {
        perror("Erreur lors de la création de la socket");
        return -1;
    }
    
    // Enregistrer la socket globalement pour le gestionnaire de signal
    global_socket_fd = server->socket_fd;
    
    // Configurer timeout sur la socket pour permettre la vérification de running
    struct timeval tv;
    tv.tv_sec = 1;  // 1 seconde
    tv.tv_usec = 0;
    if (setsockopt(server->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Erreur lors de la configuration du timeout");
        // Non fatal, continuer
    }
    
    // Configurer l'adresse du serveur
    memset(&server->server_addr, 0, sizeof(server->server_addr));
    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server->server_addr.sin_port = htons(SERVER_PORT);
    
    // Lier la socket à l'adresse du serveur
    if (bind(server->socket_fd, (struct sockaddr*)&server->server_addr, 
             sizeof(server->server_addr)) < 0) {
        perror("Erreur lors du bind");
        close(server->socket_fd);
        return -1;
    }
    
    // Initialiser le tableau des clients
    server->client_count = 0;
    memset(server->clients, 0, sizeof(server->clients));
    
    // Initialiser le mutex pour la liste des clients
    if (pthread_mutex_init(&server->clients_mutex, NULL) != 0) {
        perror("Erreur lors de l'initialisation du mutex");
        close(server->socket_fd);
        return -1;
    }
    
    // Configuration du gestionnaire de signal pour CTRL+C
    signal(SIGINT, handle_sigint);
    
    return 0;
}

int find_client_by_username(Server *server, const char *username) {
    int i;
    for (i = 0; i < server->client_count; i++) {
        if (strcmp(server->clients[i].username, username) == 0 && 
            server->clients[i].connected) {
            return i;
        }
    }
    return -1;
}

int add_client(Server *server, const char *username, const char *password, 
               struct sockaddr_in *addr) {
    pthread_mutex_lock(&server->clients_mutex);
    
    // Vérifier si le client existe déjà
    int idx = find_client_by_username(server, username);
    if (idx >= 0) {
        // Mettre à jour l'adresse et marquer comme connecté
        memcpy(&server->clients[idx].addr, addr, sizeof(struct sockaddr_in));
        server->clients[idx].connected = true;
        
        pthread_mutex_unlock(&server->clients_mutex);
        return idx;
    }
    
    // Ajouter un nouveau client
    if (server->client_count < MAX_CLIENTS) {
        idx = server->client_count++;
        
        strncpy(server->clients[idx].username, username, sizeof(server->clients[idx].username) - 1);
        server->clients[idx].username[sizeof(server->clients[idx].username) - 1] = '\0';
        
        strncpy(server->clients[idx].password, password, sizeof(server->clients[idx].password) - 1);
        server->clients[idx].password[sizeof(server->clients[idx].password) - 1] = '\0';
        
        memcpy(&server->clients[idx].addr, addr, sizeof(struct sockaddr_in));
        server->clients[idx].connected = true;
        
        pthread_mutex_unlock(&server->clients_mutex);
        return idx;
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
    return -1;
}

void remove_client(Server *server, const char *username) {
    pthread_mutex_lock(&server->clients_mutex);
    
    int idx = find_client_by_username(server, username);
    if (idx >= 0) {
        // Marquer comme déconnecté plutôt que de supprimer
        server->clients[idx].connected = false;
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
}

int send_response(Server *server, Request *res, struct sockaddr_in *client_addr) {
    // Envoyer la réponse au client
    ssize_t sent = sendto(server->socket_fd, res, sizeof(Request), 0,
                          (struct sockaddr*)client_addr, sizeof(struct sockaddr_in));
    
    if (sent < 0) {
        perror("Erreur lors de l'envoi de la réponse");
        return -1;
    }
    
    return 0;
}

void process_request(Server *server, Request *req, struct sockaddr_in *client_addr) {
    Request response;
    
    switch (req->type) {
        case REQ_CONNECT: {
            // Format attendu: "username password"
            char username[50];
            char password[50];
            
            if (sscanf(req->content, "%49s %49s", username, password) != 2) {
                // Format invalide
                init_request(&response, REQ_MESSAGE, "Server", "", 
                             "Format de connexion invalide");
                send_response(server, &response, client_addr);
                return;
            }
            
            // Ajouter/mettre à jour le client
            int client_idx = add_client(server, username, password, client_addr);
            
            if (client_idx >= 0) {
                printf("Client connecté: %s\n", username);
                
                // Envoyer une confirmation
                init_request(&response, REQ_MESSAGE, "Server", "", 
                             "Connexion réussie");
                send_response(server, &response, client_addr);
                
                // Annoncer la connexion aux autres clients
                char announce[100];
                sprintf(announce, "%s a rejoint le chat", username);
                init_request(&response, REQ_MESSAGE, "Server", "", announce);
                
                pthread_mutex_lock(&server->clients_mutex);
                for (int i = 0; i < server->client_count; i++) {
                    if (i != client_idx && server->clients[i].connected) {
                        send_response(server, &response, &server->clients[i].addr);
                    }
                }
                pthread_mutex_unlock(&server->clients_mutex);
            } else {
                // Échec de connexion (serveur plein)
                init_request(&response, REQ_MESSAGE, "Server", "", 
                             "Échec de connexion: serveur plein");
                send_response(server, &response, client_addr);
            }
            break;
        }
        
        case REQ_DISCONNECT: {
            // Marquer le client comme déconnecté
            remove_client(server, req->sender);
            printf("Client déconnecté: %s\n", req->sender);
            
            // Envoyer un ACK de déconnexion au client
            init_request(&response, REQ_MESSAGE, "Server", req->sender, "Déconnexion confirmée");
            send_response(server, &response, client_addr);
            
            // Annoncer la déconnexion aux autres clients
            char announce[100];
            sprintf(announce, "%s a quitté le chat", req->sender);
            init_request(&response, REQ_MESSAGE, "Server", "", announce);
            
            pthread_mutex_lock(&server->clients_mutex);
            for (int i = 0; i < server->client_count; i++) {
                if (strcmp(server->clients[i].username, req->sender) != 0 && 
                    server->clients[i].connected) {
                    send_response(server, &response, &server->clients[i].addr);
                }
            }
            pthread_mutex_unlock(&server->clients_mutex);
            break;
        }
        
        case REQ_MESSAGE: {
            // Diffuser le message à tous les clients
            printf("Message de %s: %s\n", req->sender, req->content);
            
            pthread_mutex_lock(&server->clients_mutex);
            for (int i = 0; i < server->client_count; i++) {
                if (strcmp(server->clients[i].username, req->sender) != 0 && 
                    server->clients[i].connected) {
                    send_response(server, req, &server->clients[i].addr);
                }
            }
            pthread_mutex_unlock(&server->clients_mutex);
            break;
        }
        
        case REQ_COMMAND: {
            printf("Commande de %s: %s\n", req->sender, req->content);
            
            CommandResult result = process_command(server, req, client_addr);
            
            if (result == CMD_SHUTDOWN) {
                // La commande shutdown a été exécutée, le serveur va s'arrêter
                running = 0;
            }
            break;
        }
        
        default:
            // Type de requête inconnu
            init_request(&response, REQ_MESSAGE, "Server", "", 
                         "Type de requête non pris en charge");
            send_response(server, &response, client_addr);
            break;
    }
}

void *receive_messages_thread(void *arg) {
    Server *server = (Server *)arg;
    Request req;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    // Boucle pour recevoir des messages
    while (running) {
        // Recevoir une requête d'un client
        ssize_t received = recvfrom(server->socket_fd, &req, sizeof(Request), 0,
                                   (struct sockaddr*)&client_addr, &client_len);
        
        if (received < 0) {
            // Si running est à 0, on termine la boucle
            if (!running) {
                break;
            }
            
            // Si c'est un timeout, on continue la boucle
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            
            // Sinon, c'est une vraie erreur
            perror("Erreur lors de la réception de la requête");
            continue;
        }
        
        // Traiter la requête
        process_request(server, &req, &client_addr);
    }
    
    printf("Thread de réception du serveur terminé.\n");
    return NULL;
}

// Fonction principale
int main(void) {  // Suppression des paramètres inutilisés
    Server server;
    
    // Initialiser le serveur
    if (init_server(&server) < 0) {
        return EXIT_FAILURE;
    }
    
    printf("Serveur démarré sur le port %d\n", SERVER_PORT);
    printf("Appuyez sur Ctrl+C pour arrêter le serveur.\n");
    
    init_command_system();

    // Créer le thread principal de réception
    pthread_t receive_thread;
    
    if (pthread_create(&receive_thread, NULL, receive_messages_thread, &server) != 0) {
        perror("Erreur lors de la création du thread de réception");
        close(server.socket_fd);
        pthread_mutex_destroy(&server.clients_mutex);
        return EXIT_FAILURE;
    }
    
    // Attendre que le thread se termine (lorsque running devient 0)
    pthread_join(receive_thread, NULL);
    
    // Envoyer un message de fermeture à tous les clients
    Request shutdown_notice;
    init_request(&shutdown_notice, REQ_MESSAGE, "Server", "", "Le serveur est en train de s'arrêter.");
    
    pthread_mutex_lock(&server.clients_mutex);
    for (int i = 0; i < server.client_count; i++) {
        if (server.clients[i].connected) {
            send_response(&server, &shutdown_notice, &server.clients[i].addr);
        }
    }
    pthread_mutex_unlock(&server.clients_mutex);
    
    // Nettoyage
    close(server.socket_fd);
    pthread_mutex_destroy(&server.clients_mutex);
    
    printf("Serveur arrêté proprement.\n");
    
    return EXIT_SUCCESS;
}