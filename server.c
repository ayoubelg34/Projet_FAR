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
    
    // Initialiser les salons
    server->nb_salons = 0;
    memset(server->salons, 0, sizeof(server->salons));
    pthread_mutex_init(&server->salons_mutex, NULL);

    // Charger les salons existants depuis un fichier
    load_rooms(server, "rooms.txt");

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
    // Vérifier si la socket est encore valide
    if (!running || server->socket_fd < 0) {
        return -1;
    }
    
    // Envoyer la réponse au client
    ssize_t sent = sendto(server->socket_fd, res, sizeof(Request), 0,
                          (struct sockaddr*)client_addr, sizeof(struct sockaddr_in));
    
    if (sent < 0) {
        // Ne pas afficher d'erreur si le serveur est en cours d'arrêt
        if (running) {
            perror("Erreur lors de l'envoi de la réponse");
        }
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
            int idx = find_client_by_username(server, req->sender);
            if (idx >= 0 && strlen(server->clients[idx].salon_courant) > 0) {
                const char *salon = server->clients[idx].salon_courant;
                printf("[%s] %s: %s\n", salon, req->sender, req->content);
                broadcast_room(server, salon, req, req->sender);
            } else {
                init_request(&response, REQ_MESSAGE, "Server", req->sender,
                            "Vous devez rejoindre un salon avant d'envoyer un message.");
                send_response(server, &response, client_addr);
            }
            break;
        }

        
        case REQ_COMMAND: {
            char *cmd = req->content;
            printf("Commande reçue de %s: %s\n", req->sender, cmd);

            if (strncmp(cmd, "@create ", 8) == 0) {
                if (create_room(server, cmd + 8) == 0)
                    init_request(&response, REQ_MESSAGE, "Server", "", "Salon créé avec succès.");
                else
                    init_request(&response, REQ_MESSAGE, "Server", "", "Erreur : salon existe déjà.");

            } else if (strncmp(cmd, "@join ", 6) == 0) {
                if (join_room(server, req->sender, cmd + 6) == 0)
                    init_request(&response, REQ_MESSAGE, "Server", "", "Vous avez rejoint le salon.");
                else
                    init_request(&response, REQ_MESSAGE, "Server", "", "Erreur : salon introuvable ou plein.");

            } else if (strcmp(cmd, "@leave") == 0) {
                if (remove_user(server, req->sender, NULL) == 0)
                    init_request(&response, REQ_MESSAGE, "Server", "", "Vous avez quitté le salon.");
                else
                    init_request(&response, REQ_MESSAGE, "Server", "", "Vous n'étiez dans aucun salon.");

            } else {
                init_request(&response, REQ_MESSAGE, "Server", "", "Commande inconnue.");
            }

            send_response(server, &response, client_addr);
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



int find_room(Server *server, const char *name) {
    for (int i = 0; i < server->nb_salons; i++) {
        if (strcmp(server->salons[i].nom, name) == 0)
            return i;
    }
    return -1;
}

int create_room(Server *server, const char *name) {
    pthread_mutex_lock(&server->salons_mutex);
    if (find_room(server, name) >= 0 || server->nb_salons >= MAX_SALONS) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -1;
    }
    Salon *room = &server->salons[server->nb_salons++];
    strncpy(room->nom, name, MAX_NOM_SALON - 1);
    room->nb_membres = 0;
    pthread_mutex_unlock(&server->salons_mutex);
    return 0;
}

int join_room(Server *server, const char *username, const char *room_name) {
    int idx = find_client_by_username(server, username);
    if (idx < 0) return -1;

    remove_user(server, username, NULL);

    pthread_mutex_lock(&server->salons_mutex);
    int rid = find_room(server, room_name);
    if (rid < 0 || server->salons[rid].nb_membres >= MAX_MEMBRES) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -1;
    }

    Salon *room = &server->salons[rid];
    strncpy(room->membres[room->nb_membres++], username, 49);
    pthread_mutex_unlock(&server->salons_mutex);

    pthread_mutex_lock(&server->clients_mutex);
    strncpy(server->clients[idx].salon_courant, room_name, MAX_NOM_SALON);
    pthread_mutex_unlock(&server->clients_mutex);

    return 0;
}

int remove_user(Server *server, const char *username, const char *room_name) {
    int cid = find_client_by_username(server, username);
    if (cid < 0) return -1;

    const char *room = room_name ? room_name : server->clients[cid].salon_courant;
    if (!room || strlen(room) == 0) return -1;

    pthread_mutex_lock(&server->salons_mutex);
    int rid = find_room(server, room);
    if (rid < 0) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -1;
    }

    Salon *s = &server->salons[rid];
    for (int i = 0; i < s->nb_membres; i++) {
        if (strcmp(s->membres[i], username) == 0) {
            for (int j = i; j < s->nb_membres - 1; j++)
                strcpy(s->membres[j], s->membres[j + 1]);
            s->nb_membres--;
            break;
        }
    }
    pthread_mutex_unlock(&server->salons_mutex);

    pthread_mutex_lock(&server->clients_mutex);
    server->clients[cid].salon_courant[0] = '\0';
    pthread_mutex_unlock(&server->clients_mutex);

    return 0;
}

void broadcast_room(Server *server, const char *room, Request *msg, const char *sender) {
    int rid = find_room(server, room);
    if (rid < 0) return;

    Salon *r = &server->salons[rid];
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < r->nb_membres; i++) {
        if (strcmp(r->membres[i], sender) != 0) {
            int cid = find_client_by_username(server, r->membres[i]);
            if (cid >= 0 && server->clients[cid].connected) {
                send_response(server, msg, &server->clients[cid].addr);
            }
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
}

void save_rooms(Server *server, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        perror("Erreur ouverture fichier rooms.txt");
        return;
    }

    pthread_mutex_lock(&server->salons_mutex);
    for (int i = 0; i < server->nb_salons; i++) {
        Salon *s = &server->salons[i];
        if (s->nb_membres == 0) continue; // Ne sauvegarde que les salons avec au moins 1 membre

        fprintf(f, "salon: %s\n", s->nom);
        for (int j = 0; j < s->nb_membres; j++) {
            fprintf(f, "membre: %s\n", s->membres[j]);
        }
    }
    pthread_mutex_unlock(&server->salons_mutex);
    fclose(f);
}

void load_rooms(Server *server, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return;

    char line[100];
    Salon *current = NULL;

    pthread_mutex_lock(&server->salons_mutex);
    server->nb_salons = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "salon: ", 7) == 0) {
            if (server->nb_salons >= MAX_SALONS) break;
            current = &server->salons[server->nb_salons++];
            sscanf(line + 7, "%49[^\n]", current->nom);
            current->nb_membres = 0;
        } else if (strncmp(line, "membre: ", 8) == 0 && current) {
            if (current->nb_membres < MAX_MEMBRES) {
                sscanf(line + 8, "%49[^\n]", current->membres[current->nb_membres++]);
            }
        }
    }

    pthread_mutex_unlock(&server->salons_mutex);
    fclose(f);
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

    // Sauvegarder les salons avant de quitter
    save_rooms(&server, "rooms.txt");
    
    // Nettoyage - fermer la socket seulement après avoir envoyé tous les messages
    close(server.socket_fd);
    global_socket_fd = -1; // Réinitialisation pour éviter une double fermeture
    pthread_mutex_destroy(&server.clients_mutex);
    pthread_mutex_destroy(&server.salons_mutex);
    
    printf("Serveur arrêté proprement.\n");
    
    return EXIT_SUCCESS;
}