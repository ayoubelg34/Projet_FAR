#include "server.h"
#include "command.h"
#include "common.h"

// External variables defined in common.c
extern volatile sig_atomic_t running;
extern int global_socket_fd;

// Server-specific data for signal handler
static Server *global_server = NULL;

// Server-specific signal handler
void server_sigint_handler(int sig) {
    printf("\nInterruption reçue (signal %d). Arrêt du serveur en cours...\n", sig);
    
    // Set running flag to 0
    running = 0;
    
    // Let main thread handle the shutdown procedures
    // This will allow for client notification and proper cleanup
}

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
    struct sigaction sa;
    sa.sa_handler = server_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
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
    int idx = -1;
    for (int i = 0; i < server->client_count; i++) {
        if (strcmp(server->clients[i].username, username) == 0) {
            idx = i;
            break;
        }
    }
    
    if (idx >= 0) {
        // Utilisateur trouvé - vérifier s'il est déjà connecté
        if (server->clients[idx].connected) {
            pthread_mutex_unlock(&server->clients_mutex);
            return -2; // Code d'erreur : utilisateur déjà connecté
        }
        
        // Vérifier le mot de passe pour reconnexion
        if (strcmp(server->clients[idx].password, password) != 0) {
            pthread_mutex_unlock(&server->clients_mutex);
            return -3; // Code d'erreur : mot de passe incorrect
        }
        
        // Reconnexion autorisée - mettre à jour l'adresse
        memcpy(&server->clients[idx].addr, addr, sizeof(struct sockaddr_in));
        server->clients[idx].connected = true;
        
        // Vérifier si la période de mute est terminée
        if (server->clients[idx].is_muted) {
            time_t now = time(NULL);
            if (now >= server->clients[idx].mute_until) {
                // Le mute a expiré
                server->clients[idx].is_muted = false;
                server->clients[idx].mute_until = 0;
                printf("Le mode muet de l'utilisateur %s a expiré pendant son absence\n", username);
            }
        }
        
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

    // Pour un nouveau client
    if (server->client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&server->clients_mutex);
        return -4; // Serveur plein
    }
    
    // Ajouter le nouveau client
    idx = server->client_count++;
    
    strncpy(server->clients[idx].username, username, sizeof(server->clients[idx].username) - 1);
    server->clients[idx].username[sizeof(server->clients[idx].username) - 1] = '\0';
    
    strncpy(server->clients[idx].password, password, sizeof(server->clients[idx].password) - 1);
    server->clients[idx].password[sizeof(server->clients[idx].password) - 1] = '\0';
    
    memcpy(&server->clients[idx].addr, addr, sizeof(struct sockaddr_in));
    server->clients[idx].connected = true;
    server->clients[idx].salon_courant[0] = '\0';
    
    // Initialiser les champs relatifs au mute
    server->clients[idx].is_muted = false;
    server->clients[idx].mute_until = 0;
    
    // Définir le rôle par défaut comme utilisateur
    // Premier utilisateur est automatiquement admin
    if (server->client_count == 1) {
        server->clients[idx].role = ROLE_ADMIN;
    } else {
        server->clients[idx].role = ROLE_USER;
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
    return idx;
}

// Fonction save_users_to_file et load_users_from_file pour sauvegarder et charger les utilisateurs
void save_users_to_file(Server *server) {
    FILE *file = fopen("users.dat", "wb");
    if (!file) {
        perror("Erreur lors de l'ouverture du fichier de sauvegarde");
        return;
    }
    
    // Verrouiller la mutex pour accéder à la liste des clients
    pthread_mutex_lock(&server->clients_mutex);
    
    // Écrire le nombre de clients
    fwrite(&server->client_count, sizeof(int), 1, file);
    
    // Écrire les informations de chaque client
    for (int i = 0; i < server->client_count; i++) {
        fwrite(&server->clients[i].username, sizeof(server->clients[i].username), 1, file);
        fwrite(&server->clients[i].password, sizeof(server->clients[i].password), 1, file);
        fwrite(&server->clients[i].role, sizeof(UserRole), 1, file);
        fwrite(&server->clients[i].is_muted, sizeof(bool), 1, file);
        fwrite(&server->clients[i].mute_until, sizeof(time_t), 1, file);
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
    fclose(file);
    printf("Utilisateurs sauvegardés avec succès\n");
}

void load_users_from_file(Server *server) {
    FILE *file = fopen("users.dat", "rb");
    if (!file) {
        printf("Aucun fichier d'utilisateurs trouvé. Un nouveau sera créé.\n");
        return;
    }
    
    // Lire le nombre de clients
    int count;
    if (fread(&count, sizeof(int), 1, file) != 1) {
        perror("Erreur lors de la lecture du nombre d'utilisateurs");
        fclose(file);
        return;
    }
    
    // Verrouiller la mutex
    pthread_mutex_lock(&server->clients_mutex);
    
    // Lire les informations de chaque client
    for (int i = 0; i < count && i < MAX_CLIENTS; i++) {
        ClientInfo client;
        client.connected = false;  // Par défaut non connectés au démarrage
        
        if (fread(&client.username, sizeof(client.username), 1, file) != 1 ||
            fread(&client.password, sizeof(client.password), 1, file) != 1 ||
            fread(&client.role, sizeof(UserRole), 1, file) != 1) {
            perror("Erreur lors de la lecture des informations d'un utilisateur");
            break;
        }
        
        // Tentative de lecture des nouveaux champs (compatibilité avec anciennes versions)
        client.is_muted = false;
        client.mute_until = 0;
        
        fread(&client.is_muted, sizeof(bool), 1, file);
        fread(&client.mute_until, sizeof(time_t), 1, file);
        
        // Vérifier si un utilisateur encore "muet" devrait toujours l'être
        if (client.is_muted) {
            time_t now = time(NULL);
            if (now >= client.mute_until) {
                client.is_muted = false;
                printf("Le mode muet de l'utilisateur %s a expiré\n", client.username);
            } else {
                int minutes_left = (int)((client.mute_until - now) / 60) + 1;
                printf("L'utilisateur %s est muet pour encore %d minute(s)\n", 
                       client.username, minutes_left);
            }
        }
        
        // Copier les informations dans le tableau des clients
        memcpy(&server->clients[i], &client, sizeof(ClientInfo));
        server->client_count = i + 1;
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
    fclose(file);
    printf("%d utilisateurs chargés depuis le fichier\n", server->client_count);
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
    
    // Configurer un timeout pour que la socket ne bloque pas indéfiniment
    struct timeval tv;
    tv.tv_sec = 5;  // 5 secondes timeout
    tv.tv_usec = 0;
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Erreur lors de la configuration du timeout");
        // Non fatal, continuer
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
    
    // Configuration du mode non-bloquant
    fcntl(server_socket, F_SETFL, fcntl(server_socket, F_GETFL, 0) | O_NONBLOCK);
    
    while (running) {
        // Vérifier périodiquement si le serveur doit s'arrêter
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        // Configurer un timeout court pour select
        struct timeval select_timeout;
        select_timeout.tv_sec = 1;
        select_timeout.tv_usec = 0;
        
        // Utiliser select pour vérifier si des données sont disponibles
        int ready = select(server_socket + 1, &read_fds, NULL, NULL, &select_timeout);
        
        if (ready < 0) {
            if (errno == EINTR) {
                // Interruption par un signal (comme SIGINT)
                if (!running) break;
                continue;
            }
            perror("Erreur lors de select");
            continue;
        }
        
        // Si aucune activité, vérifier si le serveur doit s'arrêter
        if (ready == 0) {
            if (!running) break;
            continue;
        }
        
        // Accepter une connexion seulement si des données sont disponibles
        if (FD_ISSET(server_socket, &read_fds)) {
            client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Aucune connexion disponible, continuer
                    continue;
                }
                
                if (!running) break;
                
                perror("Erreur lors de l'acceptation de la connexion");
                continue;
            }
            
            printf("Nouvelle connexion de fichier depuis %s:%d\n", 
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            // Configurer un timeout pour cette socket client aussi
            if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                perror("Erreur lors de la configuration du timeout client");
                // Non fatal, continuer
            }
            
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
            
            // Générer un nom unique si le fichier existe déjà
            char unique_filename[256];
            generate_unique_filename(upload_dir, filename, unique_filename, sizeof(unique_filename));
            
            // Construire le chemin complet du fichier
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", upload_dir, unique_filename);
            
            // Si le nom a été modifié, informer l'utilisateur
            if (strcmp(filename, unique_filename) != 0) {
                printf("Le fichier existe déjà. Renommé en %s\n", unique_filename);
            }
            
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
            
            // Recevoir et enregistrer le contenu du fichier avec vérification régulière de running
            ssize_t bytes_read;
            fd_set recv_fds;
            struct timeval recv_timeout;
            
            while (running) {
                // Configurer select pour cette socket
                FD_ZERO(&recv_fds);
                FD_SET(client_socket, &recv_fds);
                recv_timeout.tv_sec = 1; // Vérifier running toutes les secondes
                recv_timeout.tv_usec = 0;
                
                int recv_ready = select(client_socket + 1, &recv_fds, NULL, NULL, &recv_timeout);
                
                if (recv_ready < 0) {
                    if (errno == EINTR) {
                        // Interruption par un signal
                        if (!running) break;
                        continue;
                    }
                    perror("Erreur select sur le client");
                    break;
                }
                
                // Timeout - vérifier si on doit continuer
                if (recv_ready == 0) {
                    if (!running) break;
                    continue;
                }
                
                // Données disponibles à lire
                if (FD_ISSET(client_socket, &recv_fds)) {
                    bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
                    
                    if (bytes_read <= 0) {
                        // Fin de fichier ou erreur
                        if (bytes_read < 0) {
                            perror("Erreur lors de la réception du fichier");
                        }
                        break;
                    }
                    
                    // Écrire les données dans le fichier
                    if (fwrite(buffer, 1, (size_t)bytes_read, file) != (size_t)bytes_read) {
                        perror("Erreur lors de l'écriture dans le fichier");
                        break;
                    }
                }
            }
            
            // Fermer le fichier
            fclose(file);
            
            if (running) {
                printf("Fichier reçu et enregistré: %s\n", filepath);
            } else {
                printf("Réception du fichier interrompue: %s\n", filepath);
            }
            
            // Fermer la socket client
            close(client_socket);
        }
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
    
    // Verify if the server should still be running
    if (!running) {
        return -1;
    }
    
    // Correct the path to point to bin/uploads
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "./uploads/%s", filename);
    
    // Log the file path we're trying to open
    printf("send_file_to_client: trying to open file: %s\n", filepath);
    
    file = fopen(filepath, "rb");
    if (file == NULL) {
        perror("Erreur lors de l'ouverture du fichier");
        return -1;
    }
    
    printf("send_file_to_client: file opened successfully\n");
    
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
    
    // Configurer un timeout sur la socket
    struct timeval tv;
    tv.tv_sec = 5;  // 5 secondes timeout
    tv.tv_usec = 0;
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Erreur lors de la configuration du timeout");
        // Non fatal, continuer
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
    
    // Configurer le mode non-bloquant pour la socket
    fcntl(tcp_socket, F_SETFL, fcntl(tcp_socket, F_GETFL, 0) | O_NONBLOCK);
    
    // Attendre la connexion du client avec un délai limité
    int client_socket = -1;
    time_t start_time = time(NULL);
    const int max_wait_time = 30; // 30 secondes max pour que le client se connecte
    
    while ((time(NULL) - start_time < max_wait_time) && running) {
        // Vérifier si une connexion est disponible
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(tcp_socket, &read_fds);
        
        struct timeval select_timeout;
        select_timeout.tv_sec = 1;  // Vérifier périodiquement si le serveur doit s'arrêter
        select_timeout.tv_usec = 0;
        
        int ready = select(tcp_socket + 1, &read_fds, NULL, NULL, &select_timeout);
        
        if (ready < 0) {
            if (errno == EINTR) {
                // Interruption par un signal
                if (!running) break;
                continue;
            }
            perror("Erreur lors de select");
            close(tcp_socket);
            fclose(file);
            return -1;
        }
        
        // Aucune connexion disponible, continuer d'attendre
        if (ready == 0) {
            if (!running) break;
            continue;
        }
        
        // Une connexion est disponible
        client_socket = accept(tcp_socket, (struct sockaddr*)client_addr, &server_len);
        if (client_socket < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Aucune connexion disponible, continuer d'attendre
                continue;
            }
            perror("Erreur lors de l'acceptation de la connexion");
            close(tcp_socket);
            fclose(file);
            return -1;
        }
        
        // Connexion établie, sortir de la boucle
        break;
    }
    
    // Vérifier si la connexion a été établie
    if (client_socket < 0 || !running) {
        close(tcp_socket);
        fclose(file);
        if (!running) {
            printf("Envoi du fichier annulé: arrêt du serveur\n");
        } else {
            printf("Timeout lors de l'attente de la connexion du client\n");
        }
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
    
    // Attendre l'ACK du client avec un timeout
    char ack_buffer[10] = {0};
    fd_set ack_fds;
    FD_ZERO(&ack_fds);
    FD_SET(client_socket, &ack_fds);
    
    struct timeval ack_timeout;
    ack_timeout.tv_sec = 5;  // 5 secondes pour recevoir l'ACK
    ack_timeout.tv_usec = 0;
    
    if (select(client_socket + 1, &ack_fds, NULL, NULL, &ack_timeout) <= 0 || !running) {
        perror("Timeout lors de l'attente de l'ACK");
        close(client_socket);
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    if (recv(client_socket, ack_buffer, sizeof(ack_buffer), 0) <= 0) {
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
    
    // Envoyer le contenu du fichier avec vérification périodique de running
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0 && running) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(client_socket, &write_fds);
        
        struct timeval write_timeout;
        write_timeout.tv_sec = 1;  // Vérifier running toutes les secondes
        write_timeout.tv_usec = 0;
        
        // Attendre que la socket soit prête pour écrire
        int write_ready = select(client_socket + 1, NULL, &write_fds, NULL, &write_timeout);
        
        if (write_ready < 0) {
            if (errno == EINTR) {
                // Interruption par un signal
                if (!running) break;
                continue;
            }
            perror("Erreur select lors de l'envoi");
            break;
        }
        
        // Timeout - vérifier si on doit continuer
        if (write_ready == 0) {
            if (!running) break;
            continue;
        }
        
        // Socket prête pour écrire
        if (send(client_socket, buffer, bytes_read, 0) < 0) {
            perror("Erreur lors de l'envoi du fichier");
            close(client_socket);
            close(tcp_socket);
            fclose(file);
            return -1;
        }
    }
    
    if (running) {
        printf("Fichier envoyé avec succès à %s.\n", inet_ntoa(client_addr->sin_addr));
    } else {
        printf("Envoi du fichier interrompu: arrêt du serveur\n");
    }
    
    // Fermer les sockets et le fichier
    close(client_socket);
    close(tcp_socket);
    fclose(file);
    
    return running ? 0 : -1; // Succès seulement si le serveur était toujours en cours d'exécution
}

// Thread pour l'envoi de fichier
void *file_send_thread_func(void *arg) {
    FileTransferArgs *args = (FileTransferArgs *)arg;
    
    printf("Démarrage du thread d'envoi de fichier pour %s\n", args->filename);
    
    // Envoyer le fichier
    int result = send_file_to_client(args->filename, &args->client_addr);
    
    // Stocker le résultat
    args->success = (result == 0);
    if (!args->success) {
        strncpy(args->message, "Échec de l'envoi du fichier", sizeof(args->message) - 1);
        args->message[sizeof(args->message) - 1] = '\0';
    } else {
        strncpy(args->message, "Fichier envoyé avec succès", sizeof(args->message) - 1);
        args->message[sizeof(args->message) - 1] = '\0';
    }
    
    // Notifier le client du résultat via UDP
    Request notification;
    char notification_content[MAX_MSG_SIZE];
    
    if (args->success) {
        snprintf(notification_content, sizeof(notification_content), 
                 "Fichier %s envoyé avec succès", args->filename);
    } else {
        snprintf(notification_content, sizeof(notification_content), 
                 "Échec de l'envoi du fichier %s", args->filename);
    }
    
    init_request(&notification, REQ_MESSAGE, "Server", "", notification_content);
    
    Server *server = (Server *)pthread_getspecific(server_key);
    if (server) {
        send_response(server, &notification, &args->client_addr);
    } else {
        // Fallback si le serveur n'est pas accessible via pthread_getspecific
        sendto(global_socket_fd, &notification, sizeof(Request), 0,
               (struct sockaddr*)&args->client_addr, sizeof(struct sockaddr_in));
    }
    
    return args;
}

// Clé pour stocker le pointeur serveur dans les threads
pthread_key_t server_key;


void process_request(Server *server, Request *req, struct sockaddr_in *client_addr) {
    // Stocker le pointeur serveur pour les threads de fichier
    pthread_setspecific(server_key, server);
    
    Request response;
    
    // Pour les messages normaux ou les commandes, vérifier si l'utilisateur est muet
    if (req->type == REQ_MESSAGE || req->type == REQ_COMMAND) {
        pthread_mutex_lock(&server->clients_mutex);
        int client_idx = find_client_by_username(server, req->sender);
        
        if (client_idx >= 0 && server->clients[client_idx].is_muted) {
            // Vérifier si la période de mute est terminée
            time_t now = time(NULL);
            if (now < server->clients[client_idx].mute_until) {
                // Calculer le temps restant
                int minutes_left = (int)((server->clients[client_idx].mute_until - now) / 60) + 1;
                
                pthread_mutex_unlock(&server->clients_mutex);
                
                // Si ce n'est pas une commande @help ou @credits (qu'on autorise même en mode muet)
                if (req->type != REQ_COMMAND || 
                    (strncmp(req->content, "@help", 5) != 0 && 
                     strncmp(req->content, "@credits", 8) != 0 &&
                     strncmp(req->content, "@disconnect", 11) != 0)) {
                    // Informer l'utilisateur qu'il est muet
                    char mute_msg[128];
                    snprintf(mute_msg, sizeof(mute_msg), 
                             "Vous êtes actuellement en mode muet. Vous pourrez parler à nouveau dans %d minute(s).", 
                             minutes_left);
                    init_request(&response, REQ_MESSAGE, "Server", "", mute_msg);
                    send_response(server, &response, client_addr);
                    return;
                }
            } else {
                // La période de mute est terminée, réactiver l'utilisateur
                server->clients[client_idx].is_muted = false;
                server->clients[client_idx].mute_until = 0;
                
                // Notifier l'utilisateur
                pthread_mutex_unlock(&server->clients_mutex);
                
                char notify[128];
                snprintf(notify, sizeof(notify), "Votre mode muet est terminé. Vous pouvez à nouveau parler.");
                init_request(&response, REQ_MESSAGE, "Server", "", notify);
                send_response(server, &response, client_addr);
                
                // Continuer le traitement normal du message
            }
        } else {
            pthread_mutex_unlock(&server->clients_mutex);
        }
    }
    
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
            
            // Tenter d'ajouter/connecter le client
            int result = add_client(server, username, password, client_addr);
            
            switch (result) {
                case -2: // Utilisateur déjà connecté
                    printf("Tentative de connexion refusée: %s (déjà connecté)\n", username);
                    init_request(&response, REQ_MESSAGE, "Server", "", 
                                 "Erreur: Ce pseudonyme est déjà utilisé par un autre utilisateur connecté");
                    send_response(server, &response, client_addr);
                    break;
                    
                case -3: // Mot de passe incorrect
                    printf("Tentative de connexion refusée: %s (mot de passe incorrect)\n", username);
                    init_request(&response, REQ_MESSAGE, "Server", "", 
                                 "Erreur: Mot de passe incorrect");
                    send_response(server, &response, client_addr);
                    break;
                    
                case -4: // Serveur plein
                    printf("Tentative de connexion refusée: %s (serveur plein)\n", username);
                    init_request(&response, REQ_MESSAGE, "Server", "", 
                                 "Erreur: Serveur plein");
                    send_response(server, &response, client_addr);
                    break;
                    
                default: // Connexion réussie
                    if (result >= 0) {
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
                            if (i != result && server->clients[i].connected) {
                                send_response(server, &response, &server->clients[i].addr);
                            }
                        }
                        pthread_mutex_unlock(&server->clients_mutex);
                    }
                    break;
            }
            break;
        }
        
        case REQ_DISCONNECT: {
            // Marquer le client comme déconnecté
            int client_idx = find_client_by_username(server, req->sender);
            if (client_idx >= 0) {
                pthread_mutex_lock(&server->clients_mutex);
                server->clients[client_idx].connected = false;
                pthread_mutex_unlock(&server->clients_mutex);
            }
            
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
                if (server->clients[i].connected && 
                    strcmp(server->clients[i].username, req->sender) != 0) {
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
            // Toutes les commandes sont maintenant traitées par le système de commandes
            CommandResult result = process_command(server, req, client_addr);
            
            if (result == CMD_SHUTDOWN) {
                // The shutdown command was executed, the server will stop
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

int is_client_still_connected(Server *server, int client_idx) {
    struct sockaddr_in *addr = &server->clients[client_idx].addr;
    
    // Créer un message de vérification
    Request ping_req;
    init_request(&ping_req, REQ_MESSAGE, "Server", "", "keepalive");
    
    // Tenter d'envoyer un message de vérification
    ssize_t sent = sendto(server->socket_fd, &ping_req, sizeof(Request), 0,
                         (struct sockaddr*)addr, sizeof(struct sockaddr_in));
    
    return (sent > 0);
}

// Fonction pour nettoyer les clients déconnectés
void cleanup_disconnected_clients(Server *server) {
    pthread_mutex_lock(&server->clients_mutex);
    
    for (int i = 0; i < server->client_count; i++) {
        if (server->clients[i].connected) {
            // Vous pouvez utiliser cette fonction pour vérifier périodiquement
            // la connectivité des clients si nécessaire
        }
    }
    
    pthread_mutex_unlock(&server->clients_mutex);
}

// Fonction principale
int main(void) {
    Server server;
    
    // Initialiser le serveur
    if (init_server(&server) < 0) {
        return EXIT_FAILURE;
    }
    
    // Charger les utilisateurs depuis le fichier
    load_users_from_file(&server);
    
    printf("Serveur démarré sur le port %d\n", SERVER_PORT);
    printf("Appuyez sur Ctrl+C pour arrêter le serveur.\n");
    
    init_command_system();

    // Initialiser la clé pour stocker le serveur dans les threads
    if (pthread_key_create(&server_key, NULL) != 0) {
        perror("Erreur lors de la création de la clé thread");
        return EXIT_FAILURE;
    }
    
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
    pthread_mutex_unlock(&server.clients_mutex);    // Sauvegarder les salons avant de quitter
        // Sauvegarde des utilisateurs APRÈS avoir déverrouillé le mutex
        save_users_to_file(&server);
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
    pthread_key_delete(server_key);
    
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