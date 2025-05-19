#include "server.h"
#include "common.h"

// Fonction pour envoyer une réponse à un client
int send_response(Server *server, Request *res, struct sockaddr_in *client_addr) {
    ssize_t sent = sendto(server->socket_fd, res, sizeof(Request), 0,
                         (struct sockaddr*)client_addr, sizeof(struct sockaddr_in));
    if (sent < 0) {
        perror("Erreur lors de l'envoi de la réponse");
        return -1;
    }
    return 0;
}

// Fonction pour marquer un client comme déconnecté
void remove_client(Server *server, const char *username) {
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < server->client_count; i++) {
        if (strcmp(server->clients[i].username, username) == 0) {
            server->clients[i].connected = false;
            
            // Quitter tous les salons
            remove_user(server, username, NULL);
            break;
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
}

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
    server->client_capacity = 10;
    server->client_count = 0;
    server->clients = malloc(sizeof(ClientInfo) * server->client_capacity);
    if (!server->clients) {
        perror("Erreur malloc clients");
        close(server->socket_fd);
        return -1;
    }

    memset(server->clients, 0, sizeof(ClientInfo) * server->client_capacity);
    
    // Initialiser le tableau des salons
    server->salon_capacity = 10;
    server->nb_salons = 0;
    server->salons = malloc(sizeof(Salon) * server->salon_capacity);
    if (!server->salons) {
        perror("Erreur malloc salons");
        free(server->clients);
        close(server->socket_fd);
        return -1;
    }

    memset(server->salons, 0, sizeof(Salon) * server->salon_capacity);

    // Initialiser le mutex pour la liste des clients
    if (pthread_mutex_init(&server->clients_mutex, NULL) != 0) {
        perror("Erreur lors de l'initialisation du mutex");
        close(server->socket_fd);
        return -1;
    }
      // Configuration du gestionnaire de signal pour CTRL+C
    signal(SIGINT, handle_sigint);
    
    // Initialiser le mutex pour les salons
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

    // Si le tableau est plein, doubler sa capacité
    if (server->client_count >= server->client_capacity) {
        int new_capacity = server->client_capacity * 2;
        ClientInfo *new_clients = realloc(server->clients, sizeof(ClientInfo) * new_capacity);
        if (!new_clients) {
            perror("Échec realloc clients");
            pthread_mutex_unlock(&server->clients_mutex);
            return -1;
        }
        server->clients = new_clients;
        server->client_capacity = new_capacity;
    }

    // Ajouter un nouveau client
    idx = server->client_count++;

    strncpy(server->clients[idx].username, username, sizeof(server->clients[idx].username) - 1);
    server->clients[idx].username[sizeof(server->clients[idx].username) - 1] = '\0';

    strncpy(server->clients[idx].password, password, sizeof(server->clients[idx].password) - 1);
    server->clients[idx].password[sizeof(server->clients[idx].password) - 1] = '\0';

    memcpy(&server->clients[idx].addr, addr, sizeof(struct sockaddr_in));
    server->clients[idx].connected = true;
    server->clients[idx].salon_courant[0] = '\0';

    pthread_mutex_unlock(&server->clients_mutex);
    return idx;
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
            printf("Commande reçue de %s: %s\n", req->sender, cmd);            if (strncmp(cmd, "@create ", 8) == 0) {
                if (create_room(server, cmd + 8, req->sender) == 0)
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

            } else if (strncmp(cmd, "@delete ", 8) == 0) {
                if (delete_room(server, cmd + 8, req->sender) == 0)
                    init_request(&response, REQ_MESSAGE, "Server", "", "Salon supprimé avec succès.");
                else
                    init_request(&response, REQ_MESSAGE, "Server", "", "Erreur : vous n'êtes pas le créateur du salon ou salon inexistant.");

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

int create_room(Server *server, const char *name, const char *creator) {
    pthread_mutex_lock(&server->salons_mutex);
    if (find_room(server, name) >= 0) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -1; // Salon déjà existant
    }

    // Redimensionner si nécessaire
    if (server->nb_salons >= server->salon_capacity) {
        int new_capacity = server->salon_capacity * 2;
        Salon *new_salons = realloc(server->salons, sizeof(Salon) * new_capacity);
        if (!new_salons) {
            perror("Échec realloc salons");
            pthread_mutex_unlock(&server->salons_mutex);
            return -1;
        }
        server->salons = new_salons;
        server->salon_capacity = new_capacity;
    }    Salon *room = &server->salons[server->nb_salons++];
    strncpy(room->nom, name, MAX_NOM_SALON - 1);
    strncpy(room->createur, creator, 49);
    room->createur[49] = '\0'; // Assurer que le nom du créateur est terminé par un null
    room->nb_membres = 0;
    
    // Initialisation du tableau de membres dynamique
    room->membres_capacity = 10; // Capacité initiale
    room->membres = malloc(sizeof(char*) * room->membres_capacity);
    if (!room->membres) {
        perror("Échec malloc membres du salon");
        server->nb_salons--; // Annuler la création du salon
        pthread_mutex_unlock(&server->salons_mutex);
        return -1;
    }
    
    // Initialisation de chaque pointeur de membre
    for (int i = 0; i < room->membres_capacity; i++) {
        room->membres[i] = NULL;
    }
    
    pthread_mutex_unlock(&server->salons_mutex);
    return 0;
}

int join_room(Server *server, const char *username, const char *room_name) {
    int idx = find_client_by_username(server, username);
    if (idx < 0) return -1;

    remove_user(server, username, NULL);    pthread_mutex_lock(&server->salons_mutex);
    int rid = find_room(server, room_name);
    if (rid < 0) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -1;
    }

    Salon *room = &server->salons[rid];
    
    // Vérifier si le tableau des membres doit être redimensionné
    if (room->nb_membres >= room->membres_capacity) {
        int new_capacity = room->membres_capacity * 2;
        char **new_membres = realloc(room->membres, sizeof(char*) * new_capacity);
        if (!new_membres) {
            perror("Échec realloc membres du salon");
            pthread_mutex_unlock(&server->salons_mutex);
            return -1;
        }
        room->membres = new_membres;
        
        // Initialiser les nouveaux emplacements
        for (int i = room->membres_capacity; i < new_capacity; i++) {
            room->membres[i] = NULL;
        }
        room->membres_capacity = new_capacity;
    }
    
    // Allouer de la mémoire pour le nouveau membre
    room->membres[room->nb_membres] = malloc(50 * sizeof(char)); // 50 caractères max pour un pseudo
    if (!room->membres[room->nb_membres]) {
        perror("Échec malloc membre");
        pthread_mutex_unlock(&server->salons_mutex);
        return -1;
    }
    
    // Copier le pseudo
    strncpy(room->membres[room->nb_membres++], username, 49);
    room->membres[room->nb_membres - 1][49] = '\0'; // S'assurer que c'est null-terminé
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
    }    Salon *s = &server->salons[rid];
    for (int i = 0; i < s->nb_membres; i++) {
        if (strcmp(s->membres[i], username) == 0) {
            // Libérer la mémoire du membre à retirer
            free(s->membres[i]);
            
            // Décalage des pointeurs
            for (int j = i; j < s->nb_membres - 1; j++)
                s->membres[j] = s->membres[j + 1];
            
            // Initialiser le dernier pointeur à NULL
            s->membres[s->nb_membres - 1] = NULL;
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
    }    pthread_mutex_lock(&server->salons_mutex);
    for (int i = 0; i < server->nb_salons; i++) {
        Salon *s = &server->salons[i];
        if (s->nb_membres == 0) continue; // Ne sauvegarde que les salons avec au moins 1 membre

        fprintf(f, "salon: %s\n", s->nom);
        fprintf(f, "createur: %s\n", s->createur);
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
    char member_name[50];
    Salon *current = NULL;

    pthread_mutex_lock(&server->salons_mutex);
    server->nb_salons = 0;

    while (fgets(line, sizeof(line), f)) {        if (strncmp(line, "salon: ", 7) == 0) {
            if (server->nb_salons >= server->salon_capacity) break;
            current = &server->salons[server->nb_salons++];
            sscanf(line + 7, "%49[^\n]", current->nom);
            current->nb_membres = 0;
            
            // Par défaut, le créateur est "admin" si non spécifié
            strcpy(current->createur, "admin");
            
            // Initialisation du tableau de membres dynamique
            current->membres_capacity = 10; // Capacité initiale
            current->membres = malloc(sizeof(char*) * current->membres_capacity);
            if (!current->membres) {
                perror("Échec malloc membres du salon");
                server->nb_salons--; // Annuler la création du salon
                continue;
            }
            
            // Initialisation de chaque pointeur de membre
            for (int i = 0; i < current->membres_capacity; i++) {
                current->membres[i] = NULL;
            }
        } else if (strncmp(line, "createur: ", 10) == 0 && current) {
            // Charger le créateur du salon
            sscanf(line + 10, "%49[^\n]", current->createur);
        } else if (strncmp(line, "membre: ", 8) == 0 && current) {
            // Vérifier si on doit augmenter la capacité
            if (current->nb_membres >= current->membres_capacity) {
                int new_capacity = current->membres_capacity * 2;
                char **new_membres = realloc(current->membres, sizeof(char*) * new_capacity);
                if (!new_membres) {
                    perror("Échec realloc membres");
                    continue;
                }
                current->membres = new_membres;
                
                // Initialiser les nouveaux emplacements
                for (int i = current->membres_capacity; i < new_capacity; i++) {
                    current->membres[i] = NULL;
                }
                current->membres_capacity = new_capacity;
            }
            
            // Extraire le nom du membre
            sscanf(line + 8, "%49[^\n]", member_name);
            
            // Allouer et copier le nom du membre
            current->membres[current->nb_membres] = malloc(50 * sizeof(char));
            if (!current->membres[current->nb_membres]) {
                perror("Échec malloc membre");
                continue;
            }
            
            strncpy(current->membres[current->nb_membres], member_name, 49);
            current->membres[current->nb_membres][49] = '\0';
            current->nb_membres++;
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
    pthread_mutex_unlock(&server.clients_mutex);    // Sauvegarder les salons avant de quitter
    save_rooms(&server, "rooms.txt");
    
    // Libérer la mémoire de tous les membres des salons
    for (int i = 0; i < server.nb_salons; i++) {
        Salon *s = &server.salons[i];
        for (int j = 0; j < s->nb_membres; j++) {
            if (s->membres[j]) {
                free(s->membres[j]);
                s->membres[j] = NULL;
            }
        }
        free(s->membres);
    }
    
    // Libérer le tableau de salons
    free(server.salons);
    
    // Libérer le tableau de clients
    free(server.clients);
    
    // Nettoyage - fermer la socket seulement après avoir envoyé tous les messages
    close(server.socket_fd);
    global_socket_fd = -1; // Réinitialisation pour éviter une double fermeture
    pthread_mutex_destroy(&server.clients_mutex);
    pthread_mutex_destroy(&server.salons_mutex);
    
    printf("Serveur arrêté proprement.\n");
    
    return EXIT_SUCCESS;
}

int delete_room(Server *server, const char *name, const char *username) {
    pthread_mutex_lock(&server->salons_mutex);
    
    // Trouver le salon
    int rid = find_room(server, name);
    if (rid < 0) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -1; // Salon inexistant
    }
    
    // Vérifier si l'utilisateur est le créateur du salon
    if (strcmp(server->salons[rid].createur, username) != 0) {
        pthread_mutex_unlock(&server->salons_mutex);
        return -2; // Pas le créateur du salon
    }
    
    Salon *salon = &server->salons[rid];
    
    // Informer tous les membres que le salon est supprimé
    pthread_mutex_lock(&server->clients_mutex);
    for (int i = 0; i < salon->nb_membres; i++) {
        int cid = find_client_by_username(server, salon->membres[i]);
        if (cid >= 0 && server->clients[cid].connected) {
            // Effacer le nom du salon courant
            if (strcmp(server->clients[cid].salon_courant, name) == 0) {
                server->clients[cid].salon_courant[0] = '\0';
            }
        }
    }
    pthread_mutex_unlock(&server->clients_mutex);
    
    // Libérer la mémoire des membres
    for (int i = 0; i < salon->nb_membres; i++) {
        if (salon->membres[i]) {
            free(salon->membres[i]);
        }
    }
    free(salon->membres);
    
    // Supprimer le salon en décalant tous les salons suivants
    for (int i = rid; i < server->nb_salons - 1; i++) {
        server->salons[i] = server->salons[i + 1];
    }
    server->nb_salons--;
    
    pthread_mutex_unlock(&server->salons_mutex);
    
    // Enregistrer les modifications dans le fichier
    save_rooms(server, "rooms.txt");
    
    return 0;
}