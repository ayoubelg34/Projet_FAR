// server.c
#include "server.h"

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

// Fonction pour gérer le transfert de fichiers via TCP
void *file_transfer_thread(void *arg) {
    (void)arg; // Pour éviter l'avertissement de paramètre non utilisé
    
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[1024];
    
    // Créer une socket TCP
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Erreur lors de la création de la socket TCP");
        return NULL;
    }
    
    // Configurer l'adresse du serveur
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(FILE_TRANSFER_PORT);
    
    // Réutiliser l'adresse
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Erreur lors de la configuration de la socket TCP");
        close(server_socket);
        return NULL;
    }
    
    // Lier la socket à l'adresse
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erreur lors du bind TCP");
        close(server_socket);
        return NULL;
    }
    
    // Écouter les connexions entrantes
    if (listen(server_socket, 5) < 0) {
        perror("Erreur lors de l'écoute TCP");
        close(server_socket);
        return NULL;
    }
    
    printf("Serveur de fichiers TCP démarré sur le port %d\n", FILE_TRANSFER_PORT);
    
    while (running) {
        // Accepter une connexion
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (!running) break;
            perror("Erreur lors de l'acceptation de la connexion");
            continue;
        }
        
        printf("Nouvelle connexion de fichier depuis %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // Recevoir le nom du fichier
        char filename[256];
        ssize_t bytes_received = recv(client_socket, filename, sizeof(filename), 0);
        
        if (bytes_received <= 0) {
            perror("Erreur lors de la réception du nom de fichier");
            close(client_socket);
            continue;
        }
        
        // Créer le répertoire de stockage s'il n'existe pas
        char upload_dir[256] = "./uploads";
        mkdir(upload_dir, 0755);
        
        // Construire le chemin complet du fichier
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", upload_dir, filename);
        
        // Envoyer un ACK au client
        if (send(client_socket, "OK", 3, 0) < 0) {
            perror("Erreur lors de l'envoi de l'ACK");
            close(client_socket);
            continue;
        }
        
        // Ouvrir le fichier pour écriture
        FILE *file = fopen(filepath, "wb");
        if (file == NULL) {
            perror("Erreur lors de la création du fichier");
            close(client_socket);
            continue;
        }
        
        // Recevoir et enregistrer le contenu du fichier
        ssize_t bytes_read;
        while ((bytes_read = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
            if (fwrite(buffer, 1, (size_t)bytes_read, file) != (size_t)bytes_read) {
                perror("Erreur lors de l'écriture dans le fichier");
                break;
            }
        }
        
        // Fermer le fichier
        fclose(file);
        
        if (bytes_read < 0) {
            perror("Erreur lors de la réception du fichier");
        } else {
            printf("Fichier reçu et enregistré: %s\n", filepath);
        }
        
        // Fermer la socket client
        close(client_socket);
    }
    
    // Fermer la socket serveur
    close(server_socket);
    printf("Thread de transfert de fichiers terminé.\n");
    return NULL;
}

// Fonction pour envoyer un fichier à un client
int send_file_to_client(const char *filename, struct sockaddr_in *client_addr) {
    int tcp_socket;
    struct sockaddr_in server_addr;
    FILE *file;
    char buffer[1024];
    size_t bytes_read;
    
    // Ouvrir le fichier
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "./uploads/%s", filename);
    
    file = fopen(filepath, "rb");
    if (file == NULL) {
        perror("Erreur lors de l'ouverture du fichier");
        return -1;
    }
    
    // Créer une socket TCP
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("Erreur lors de la création de la socket TCP");
        fclose(file);
        return -1;
    }
    
    // Réutiliser l'adresse
    int opt = 1;
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Erreur lors de la configuration de la socket TCP");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Configurer l'adresse du serveur avec un port éphémère (0)
    // au lieu d'essayer de réutiliser FILE_TRANSFER_PORT
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(0); // Port éphémère
    
    // Lier la socket à l'adresse
    if (bind(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erreur lors du bind TCP");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Obtenir le port attribué par le système
    socklen_t server_len = sizeof(server_addr);
    if (getsockname(tcp_socket, (struct sockaddr*)&server_addr, &server_len) < 0) {
        perror("Erreur lors de l'obtention du port");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    int actual_port = ntohs(server_addr.sin_port);
    
    // Écouter les connexions entrantes
    if (listen(tcp_socket, 1) < 0) {
        perror("Erreur lors de l'écoute TCP");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    printf("En attente de connexion du client pour envoyer le fichier %s sur le port %d...\n", 
           filename, actual_port);
    
    // Envoyer une notification au client via UDP avec le port à utiliser
    Request notification;
    char notification_content[MAX_MSG_SIZE];
    snprintf(notification_content, sizeof(notification_content), "@file_ready %s %d", 
             filename, actual_port);
    init_request(&notification, REQ_COMMAND, "Server", "", notification_content);
    
    if (sendto(global_socket_fd, &notification, sizeof(Request), 0,
               (struct sockaddr*)client_addr, sizeof(struct sockaddr_in)) < 0) {
        perror("Erreur lors de l'envoi de la notification");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Accepter la connexion du client
    socklen_t client_len = sizeof(*client_addr);
    int client_socket = accept(tcp_socket, (struct sockaddr*)client_addr, &client_len);
    
    if (client_socket < 0) {
        perror("Erreur lors de l'acceptation de la connexion");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Envoyer le nom du fichier
    if (send(client_socket, filename, strlen(filename) + 1, 0) < 0) {
        perror("Erreur lors de l'envoi du nom de fichier");
        close(client_socket);
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Attendre l'ACK du client
    char ack_buffer[10];
    if (recv(client_socket, ack_buffer, sizeof(ack_buffer), 0) < 0) {
        perror("Erreur lors de la réception de l'ACK");
        close(client_socket);
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    if (strcmp(ack_buffer, "OK") != 0) {
        fprintf(stderr, "Le client a refusé le transfert de fichier\n");
        close(client_socket);
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Envoyer le contenu du fichier
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send(client_socket, buffer, bytes_read, 0) < 0) {
            perror("Erreur lors de l'envoi du fichier");
            close(client_socket);
            close(tcp_socket);
            fclose(file);
            return -1;
        }
    }
    
    printf("Fichier envoyé avec succès à %s.\n", inet_ntoa(client_addr->sin_addr));
    
    // Fermer les sockets et le fichier
    close(client_socket);
    close(tcp_socket);
    fclose(file);
    
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
            // Traiter les commandes
            printf("Commande de %s: %s\n", req->sender, req->content);
            
            // Traiter la commande d'upload de fichier
            if (strncmp(req->content, "@file_uploaded ", 14) == 0) {
                char filename[256];
                sscanf(req->content + 14, "%255s", filename);
                
                printf("Fichier %s téléchargé par %s\n", filename, req->sender);
                
                // Confirmer la réception
                init_request(&response, REQ_MESSAGE, "Server", "", 
                             "Fichier téléchargé avec succès sur le serveur");
                send_response(server, &response, client_addr);
            }
            // Traiter la commande de téléchargement
            else if (strncmp(req->content, "@download ", 10) == 0) {
                char filename[256];
                sscanf(req->content + 10, "%255s", filename);
                
                printf("Demande de téléchargement du fichier %s par %s\n", filename, req->sender);
                
                // Vérifier l'existence du fichier
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "./uploads/%s", filename);
                
                if (access(filepath, F_OK) != -1) {
                    // Le fichier existe, préparer l'envoi
                    init_request(&response, REQ_MESSAGE, "Server", "", 
                                "Préparation de l'envoi du fichier...");
                    send_response(server, &response, client_addr);
                    
                    // Lancer le transfert du fichier dans un thread séparé
                    pthread_t file_send_thread;
                    FileTransferArgs *args = malloc(sizeof(FileTransferArgs));
                    if (args) {
                        strncpy(args->filename, filename, sizeof(args->filename) - 1);
                        args->filename[sizeof(args->filename) - 1] = '\0';
                        memcpy(&args->client_addr, client_addr, sizeof(struct sockaddr_in));
                        
                        if (pthread_create(&file_send_thread, NULL, file_send_thread_func, args) != 0) {
                            perror("Erreur lors de la création du thread d'envoi de fichier");
                            free(args);
                            init_request(&response, REQ_MESSAGE, "Server", "", 
                                        "Erreur serveur lors de l'envoi du fichier");
                            send_response(server, &response, client_addr);
                        }
                        pthread_detach(file_send_thread);
                    } else {
                        init_request(&response, REQ_MESSAGE, "Server", "", 
                                    "Erreur serveur: mémoire insuffisante");
                        send_response(server, &response, client_addr);
                    }
                } else {
                    // Le fichier n'existe pas
                    init_request(&response, REQ_MESSAGE, "Server", "", 
                                "Fichier non trouvé sur le serveur");
                    send_response(server, &response, client_addr);
                }
            }
            // Autres commandes
            else {
                // Répondre au client que la commande a été reçue
                init_request(&response, REQ_MESSAGE, "Server", "", 
                            "Commande reçue mais non supportée");
                send_response(server, &response, client_addr);
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

// Thread pour l'envoi de fichier
void *file_send_thread_func(void *arg) {
    FileTransferArgs *args = (FileTransferArgs *)arg;
    
    // Envoyer le fichier
    send_file_to_client(args->filename, &args->client_addr);
    
    // Libérer la mémoire
    free(args);
    return NULL;
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
int main(void) {
    Server server;
    
    // Initialiser le serveur
    if (init_server(&server) < 0) {
        return EXIT_FAILURE;
    }
    
    printf("Serveur démarré sur le port %d\n", SERVER_PORT);
    printf("Appuyez sur Ctrl+C pour arrêter le serveur.\n");
    
    // Créer le thread de transfert de fichiers TCP
    pthread_t file_thread;
    if (pthread_create(&file_thread, NULL, file_transfer_thread, &server) != 0) {
        perror("Erreur lors de la création du thread de transfert de fichiers");
        close(server.socket_fd);
        pthread_mutex_destroy(&server.clients_mutex);
        return EXIT_FAILURE;
    }
    
    // Créer le thread principal de réception
    pthread_t receive_thread;
    
    if (pthread_create(&receive_thread, NULL, receive_messages_thread, &server) != 0) {
        perror("Erreur lors de la création du thread de réception");
        close(server.socket_fd);
        pthread_mutex_destroy(&server.clients_mutex);
        return EXIT_FAILURE;
    }
    
    // Attendre que les threads se terminent (lorsque running devient 0)
    pthread_join(receive_thread, NULL);
    pthread_join(file_thread, NULL);
    
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