// client.c
#define _POSIX_C_SOURCE 200809L
#include "client.h"
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <signal.h>

// External variables defined in common.c
extern volatile sig_atomic_t running;
extern int global_socket_fd;

// Client-specific data for signal handler
static Client *global_client = NULL;
static int signal_pipe[2] = {-1, -1}; // Pipe for unblocking fgets()

// Client-specific signal handler
void client_sigint_handler(int sig) {
    printf("\nInterruption reçue (signal %d). Déconnexion en cours...\n", sig);
    
    // Set running flag to 0
    running = 0;
    
    // Send disconnect request if client is initialized
    if (global_client != NULL) {
        Request req;
        init_request(&req, REQ_DISCONNECT, global_client->username, "", "Déconnexion (SIGINT)");
        sendto(global_client->socket_fd, &req, sizeof(Request), 0,
               (struct sockaddr*)&global_client->server_addr, 
               sizeof(global_client->server_addr));
    }
    
    // Write to signal pipe to unblock fgets
    if (signal_pipe[1] >= 0) {
        char c = 'X';
        write(signal_pipe[1], &c, 1);
    }
    
    // Let the main threads finish cleanly
}

// Déclaration de fonction pour la rendre explicite
int send_file(const char *filename, const char *server_ip);

// Thread pour l'envoi de fichier en arrière-plan
void *file_transfer_thread(void *arg) {
    FileTransferThreadArgs *args = (FileTransferThreadArgs *)arg;
    
    if (args->is_upload) {
        // Upload de fichier
        printf("\rEnvoi du fichier %s en arrière-plan...\n", args->filename);
        printf("Vous: ");
        fflush(stdout);
        
        if (send_file(args->filename, args->server_ip) == 0) {
            // Succès
            printf("\rFichier %s envoyé avec succès!\n", args->filename);
            printf("Vous: ");
            fflush(stdout);
            
            // Informer le serveur via UDP
            Client *client = (Client *)pthread_getspecific(client_key);
            if (client != NULL) {
                Request req;
                char notification[MAX_MSG_SIZE];
                snprintf(notification, sizeof(notification), "@file_uploaded %s", basename(args->filename));
                init_request(&req, REQ_COMMAND, client->username, "", notification);
                send_request(client, &req);
            }
        } else {
            // Échec
            printf("\rÉchec de l'envoi du fichier %s.\n", args->filename);
            printf("Vous: ");
            fflush(stdout);
        }
    } else {
        // Download de fichier
        printf("\rRéception du fichier depuis le port %d en arrière-plan...\n", args->port);
        printf("Vous: ");
        fflush(stdout);
        
        if (receive_file_with_port(args->save_dir, args->server_ip, args->port) == 0) {
            printf("\rFichier téléchargé avec succès dans %s\n", args->save_dir);
        } else {
            printf("\rÉchec du téléchargement du fichier.\n");
        }
        printf("Vous: ");
        fflush(stdout);
    }
    
    free(args);
    return NULL;
}

// Clé pour stocker le pointeur client dans les threads
pthread_key_t client_key;

int init_client(Client *client, const char *server_ip) {
    // Créer la socket UDP
    client->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client->socket_fd < 0) {
        perror("Erreur lors de la création de la socket");
        return -1;
    }
    
    // Enregistrer la socket globalement pour le gestionnaire de signal
    global_socket_fd = client->socket_fd;
    
    // Configurer timeout sur la socket
    struct timeval tv;
    tv.tv_sec = 1;  // 1 seconde
    tv.tv_usec = 0;
    if (setsockopt(client->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Erreur lors de la configuration du timeout");
        // Non fatal, continuer
    }
    
    // Configurer l'adresse du serveur
    memset(&client->server_addr, 0, sizeof(client->server_addr));
    client->server_addr.sin_family = AF_INET;
    client->server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, server_ip, &client->server_addr.sin_addr) <= 0) {
        perror("Adresse invalide");
        close(client->socket_fd);
        return -1;
    }
    
    // Initialiser le salon courant comme vide
    client->current_room[0] = '\0';
    
    // Le gestionnaire de signal est déjà configuré dans common.c
    // Pas besoin de réinitialiser ici
    
    return 0;
}

int connect_to_server(Client *client, const char *username, const char *password) {
    // Enregistrer les informations d'identification
    strncpy(client->username, username, sizeof(client->username) - 1);
    client->username[sizeof(client->username) - 1] = '\0';
    
    strncpy(client->password, password, sizeof(client->password) - 1);
    client->password[sizeof(client->password) - 1] = '\0';
    
    // Créer une requête de connexion
    Request req;
    char connect_msg[100];
    sprintf(connect_msg, "%s %s", username, password);
    
    init_request(&req, REQ_CONNECT, username, "", connect_msg);
    
    // Envoyer la requête au serveur
    if (send_request(client, &req) < 0) {
        return -1;
    }
    
    // Attendre la réponse du serveur
    Request response;
    struct sockaddr_in server_addr;
    socklen_t server_len = sizeof(server_addr);
    
    ssize_t received = recvfrom(client->socket_fd, &response, sizeof(Request), 0,
                              (struct sockaddr*)&server_addr, &server_len);
    
    if (received < 0) {
        perror("Erreur lors de la réception de la réponse de connexion");
        return -1;
    }
    
    // Vérifier si la connexion a été acceptée
    if (strstr(response.content, "Erreur:") != NULL) {
        printf("\n[SERVER ERROR] %s\n", response.content);
        printf("Connexion refusée. Fermeture du client.\n");
        return -1;
    }
    
    if (strstr(response.content, "Connexion réussie") != NULL) {
        printf("[SERVER] %s\n", response.content);
        
        // AJOUT: Envoyer automatiquement la commande @info pour récupérer l'état actuel
        init_request(&req, REQ_COMMAND, username, "", "@info");
        send_request(client, &req);
        
        // Petit délai pour laisser le temps au serveur de répondre
        usleep(100000); // 100ms
        
        return 0;
    }
    
    return 0;  // Connexion réussie par défaut
}

int send_request(Client *client, Request *req) {
    // Envoyer la requête au serveur
    ssize_t sent = sendto(client->socket_fd, req, sizeof(Request), 0,
                          (struct sockaddr*)&client->server_addr, 
                          sizeof(client->server_addr));
    
    if (sent < 0) {
        perror("Erreur lors de l'envoi de la requête");
        return -1;
    }
    
    return 0;
}

// Fonction pour envoyer un fichier via TCP
int send_file(const char *filename, const char *server_ip) {
    int tcp_socket;
    struct sockaddr_in server_addr;
    FILE *file;
    char buffer[1024];
    size_t bytes_read;
    
    // Ouvrir le fichier
    file = fopen(filename, "rb");
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
    
    // Configurer l'adresse du serveur
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(FILE_TRANSFER_PORT); // Port dédié aux transferts de fichiers
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Adresse IP invalide");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Se connecter au serveur
    if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erreur lors de la connexion TCP au serveur");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Envoyer le nom du fichier d'abord
    char filename_buffer[256];
    strncpy(filename_buffer, basename((char*)filename), 255);
    filename_buffer[255] = '\0';
    
    if (send(tcp_socket, filename_buffer, strlen(filename_buffer) + 1, 0) < 0) {
        perror("Erreur lors de l'envoi du nom de fichier");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Attendre la confirmation du serveur
    char ack_buffer[10];
    if (recv(tcp_socket, ack_buffer, sizeof(ack_buffer), 0) < 0) {
        perror("Erreur lors de la réception de l'ACK");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    if (strcmp(ack_buffer, "OK") != 0) {
        fprintf(stderr, "Le serveur a refusé le transfert de fichier\n");
        close(tcp_socket);
        fclose(file);
        return -1;
    }
    
    // Envoyer le contenu du fichier
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send(tcp_socket, buffer, bytes_read, 0) < 0) {
            perror("Erreur lors de l'envoi du fichier");
            close(tcp_socket);
            fclose(file);
            return -1;
        }
    }
    
    printf("Fichier envoyé avec succès.\n");
    
    // Fermer la socket et le fichier
    close(tcp_socket);
    fclose(file);
    
    return 0;
}

// Fonction pour recevoir un fichier via TCP
int receive_file(const char *save_dir, const char *server_ip) {
    int tcp_socket;
    struct sockaddr_in server_addr;
    FILE *file;
    char buffer[1024];
    ssize_t bytes_received;
    
    // Créer une socket TCP
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("Erreur lors de la création de la socket TCP");
        return -1;
    }
    
    // Configurer l'adresse du serveur
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(FILE_TRANSFER_PORT); // Port dédié aux transferts de fichiers
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Adresse IP invalide");
        close(tcp_socket);
        return -1;
    }
    
    // Se connecter au serveur
    if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erreur lors de la connexion TCP au serveur");
        close(tcp_socket);
        return -1;
    }
    
    // Recevoir le nom du fichier
    char filename[256];
    if ((bytes_received = recv(tcp_socket, filename, sizeof(filename), 0)) <= 0) {
        perror("Erreur lors de la réception du nom de fichier");
        close(tcp_socket);
        return -1;
    }
    
    // Envoyer l'ACK au serveur
    if (send(tcp_socket, "OK", 3, 0) < 0) {
        perror("Erreur lors de l'envoi de l'ACK");
        close(tcp_socket);
        return -1;
    }
    
    // Construire le chemin complet du fichier
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", save_dir, filename);
    
    // Ouvrir le fichier pour écriture
    file = fopen(filepath, "wb");
    if (file == NULL) {
        perror("Erreur lors de la création du fichier");
        close(tcp_socket);
        return -1;
    }
    
    // Recevoir et écrire le contenu du fichier
    while ((bytes_received = recv(tcp_socket, buffer, sizeof(buffer), 0)) > 0) {
        if (fwrite(buffer, 1, (size_t)bytes_received, file) != (size_t)bytes_received) {
            perror("Erreur lors de l'écriture dans le fichier");
            fclose(file);
            close(tcp_socket);
            return -1;
        }
    }
    
    if (bytes_received < 0) {
        perror("Erreur lors de la réception du fichier");
        fclose(file);
        close(tcp_socket);
        return -1;
    }
    
    printf("Fichier reçu avec succès: %s\n", filepath);
    
    // Fermer le fichier et la socket
    fclose(file);
    close(tcp_socket);
    
    return 0;
}

// Nouvelle fonction pour recevoir un fichier via TCP avec un port spécifié
int receive_file_with_port(const char *save_dir, const char *server_ip, int port) {
    int tcp_socket;
    struct sockaddr_in server_addr;
    FILE *file;
    char buffer[1024];
    ssize_t bytes_received;
    
    // Créer une socket TCP
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("Erreur lors de la création de la socket TCP");
        return -1;
    }
    
    // Configurer l'adresse du serveur avec le port spécifié
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Adresse IP invalide");
        close(tcp_socket);
        return -1;
    }
    
    // Se connecter au serveur
    if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erreur lors de la connexion TCP au serveur");
        close(tcp_socket);
        return -1;
    }
    
    // Recevoir le nom du fichier
    char filename[256];
    if ((bytes_received = recv(tcp_socket, filename, sizeof(filename), 0)) <= 0) {
        perror("Erreur lors de la réception du nom de fichier");
        close(tcp_socket);
        return -1;
    }
    
    // Générer un nom unique si le fichier existe déjà
    char unique_filename[256];
    generate_unique_filename(save_dir, filename, unique_filename, sizeof(unique_filename));
    
    // Si le nom a été modifié, informer l'utilisateur
    if (strcmp(filename, unique_filename) != 0) {
        printf("Le fichier existe déjà. Renommé en %s\n", unique_filename);
    }
    
    // Envoyer l'ACK au serveur
    if (send(tcp_socket, "OK", 3, 0) < 0) {
        perror("Erreur lors de l'envoi de l'ACK");
        close(tcp_socket);
        return -1;
    }
    
    // Créer le dossier s'il n'existe pas
    mkdir(save_dir, 0755);
    
    // Construire le chemin complet du fichier
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", save_dir, unique_filename);
    
    // Ouvrir le fichier pour écriture
    file = fopen(filepath, "wb");
    if (file == NULL) {
        perror("Erreur lors de la création du fichier");
        close(tcp_socket);
        return -1;
    }
    
    // Recevoir et écrire le contenu du fichier
    while ((bytes_received = recv(tcp_socket, buffer, sizeof(buffer), 0)) > 0) {
        if (fwrite(buffer, 1, (size_t)bytes_received, file) != (size_t)bytes_received) {
            perror("Erreur lors de l'écriture dans le fichier");
            fclose(file);
            close(tcp_socket);
            return -1;
        }
    }
    
    if (bytes_received < 0) {
        perror("Erreur lors de la réception du fichier");
        fclose(file);
        close(tcp_socket);
        return -1;
    }
    
    printf("Fichier reçu avec succès: %s\n", filepath);
    
    // Fermer le fichier et la socket
    fclose(file);
    close(tcp_socket);
    
    return 0;
}

void *send_message_thread(void *arg) {
    Client *client = (Client *)arg;
    
    // Stocker le pointeur client pour les threads de fichier
    pthread_setspecific(client_key, client);
    
    char buffer[MAX_MSG_SIZE];
    char prompt[100];
    Request req;
    int prompt_displayed = 0; // Flag to track if prompt is displayed
    
    // Boucle pour envoyer des messages
    while (running) {
        // Only display prompt if it hasn't been displayed yet
        if (!prompt_displayed) {
            // Obtenir le prompt personnalisé
            get_custom_prompt(client, prompt, sizeof(prompt));
            
            // Prompt utilisateur
            printf("%s", prompt);
            fflush(stdout);
            prompt_displayed = 1;
        }
        
        // Préparation pour la sélection
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(signal_pipe[0], &readfds);
        
        int max_fd = (STDIN_FILENO > signal_pipe[0]) ? STDIN_FILENO : signal_pipe[0];
        
        // Structure pour le timeout
        struct timeval tv;
        tv.tv_sec = 1;  // 1 seconde de timeout
        tv.tv_usec = 0;
        
        // Attendre jusqu'à ce que l'entrée soit disponible ou interruption
        int select_result = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (select_result > 0) {
            // Vérifier si l'interruption vient du signal pipe
            if (FD_ISSET(signal_pipe[0], &readfds)) {
                char c;
                read(signal_pipe[0], &c, 1); // Lire le caractère pour vider le pipe
                if (!running) break;
            }
            
            // Vérifier si l'entrée est disponible sur stdin
            if (FD_ISSET(STDIN_FILENO, &readfds)) {
                // Lire l'entrée utilisateur
                if (fgets(buffer, MAX_MSG_SIZE, stdin) == NULL) {
                    if (!running) {  // Si interruption causée par le signal
                        break;
                    }
                    continue;
                }
                
                // Reset prompt flag since we will need to display it again
                prompt_displayed = 0;
                
                // Supprimer le caractère de nouvelle ligne
                buffer[strcspn(buffer, "\n")] = '\0';
                
                // Vérifier si c'est une commande d'upload de fichier
                if (strncmp(buffer, "@upload ", 8) == 0) {
                    // Extraire le nom du fichier
                    char *filename = buffer + 8;
                    
                    // Lancer le thread de transfert de fichier
                    pthread_t file_thread;
                    FileTransferThreadArgs *args = malloc(sizeof(FileTransferThreadArgs));
                    if (args) {
                        strncpy(args->filename, filename, sizeof(args->filename) - 1);
                        args->filename[sizeof(args->filename) - 1] = '\0';
                        strncpy(args->server_ip, inet_ntoa(client->server_addr.sin_addr), sizeof(args->server_ip) - 1);
                        args->server_ip[sizeof(args->server_ip) - 1] = '\0';
                        args->port = FILE_TRANSFER_PORT;
                        args->is_upload = 1;
                        
                        printf("Préparation de l'envoi du fichier %s en arrière-plan...\n", filename);
                        
                        if (pthread_create(&file_thread, NULL, file_transfer_thread, args) != 0) {
                            perror("Erreur lors de la création du thread de transfert de fichier");
                            free(args);
                        } else {
                            pthread_detach(file_thread);
                        }
                    } else {
                        perror("Erreur d'allocation mémoire");
                    }
                }
                // Vérifier si c'est une commande de téléchargement de fichier
                else if (strncmp(buffer, "@download ", 10) == 0) {
                    // Envoyer la commande au serveur
                    init_request(&req, REQ_COMMAND, client->username, "", buffer);
                    send_request(client, &req);
                }
                // Autres commandes ou messages normaux
                else if (buffer[0] == '@') {
                    // C'est une commande
                    init_request(&req, REQ_COMMAND, client->username, "", buffer);
                    send_request(client, &req);
                    
                    if (strncmp(buffer, "@disconnect", 11) == 0) {
                        running = 0;
                    }
                } else {
                    // C'est un message normal
                    init_request(&req, REQ_MESSAGE, client->username, "", buffer);
                    send_request(client, &req);
                }
            }
        } else if (select_result < 0 && errno != EINTR) {
            // Erreur inattendue
            perror("Erreur select");
            if (!running) break;
        }
        // No need to display prompt again in timeout case (select_result == 0)
        
        // Vérifier si le thread doit se terminer
        if (!running) break;
    }
    
    // Envoyer un message de déconnexion avant de quitter
    init_request(&req, REQ_DISCONNECT, client->username, "", "Déconnexion");
    send_request(client, &req);
    
    return NULL;
}

void *receive_message_thread(void *arg) {
    Client *client = (Client *)arg;
    
    // Stocker le pointeur client pour les threads de fichier
    pthread_setspecific(client_key, client);
    
    Request response;
    Request ack_response;
    socklen_t server_len = sizeof(client->server_addr);
    
    while (running) {
        ssize_t received = recvfrom(client->socket_fd, &response, sizeof(Request), 0,
                                  (struct sockaddr*)&client->server_addr, &server_len);
        
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
            perror("Erreur lors de la réception de la réponse");
            continue;
        }
        
        // Vérifier s'il s'agit d'une notification de fichier à télécharger
        if (response.type == REQ_COMMAND && strncmp(response.content, "@file_ready ", 12) == 0) {
            // Effacer la ligne actuelle
            printf("\r                                                                               \r");
            printf("Notification: Fichier prêt à être téléchargé.\n");
            
            // Extraire le nom du fichier et le port à utiliser
            char filename[256];
            int port = FILE_TRANSFER_PORT; // Port par défaut
            
            // Essayer d'obtenir le port personnalisé
            if (sscanf(response.content, "@file_ready %255s %d", filename, &port) >= 1) {
                printf("Préparation du téléchargement du fichier %s sur le port %d en arrière-plan\n", filename, port);
            }
            
            // Télécharger le fichier en arrière-plan
            char download_dir[256] = "./downloads"; // Dossier par défaut
            
            // Créer le dossier s'il n'existe pas
            mkdir(download_dir, 0755);
            
            // Lancer le thread de téléchargement
            pthread_t download_thread;
            FileTransferThreadArgs *args = malloc(sizeof(FileTransferThreadArgs));
            if (args) {
                strncpy(args->filename, filename, sizeof(args->filename) - 1);
                args->filename[sizeof(args->filename) - 1] = '\0';
                strncpy(args->server_ip, inet_ntoa(client->server_addr.sin_addr), sizeof(args->server_ip) - 1);
                args->server_ip[sizeof(args->server_ip) - 1] = '\0';
                strncpy(args->save_dir, download_dir, sizeof(args->save_dir) - 1);
                args->save_dir[sizeof(args->save_dir) - 1] = '\0';
                args->port = port;
                args->is_upload = 0;
                
                if (pthread_create(&download_thread, NULL, file_transfer_thread, args) != 0) {
                    perror("Erreur lors de la création du thread de téléchargement");
                    free(args);
                } else {
                    pthread_detach(download_thread);
                }
            } else {
                perror("Erreur d'allocation mémoire");
            }
            
            // Réafficher le prompt
            char prompt[100];
            get_custom_prompt(client, prompt, sizeof(prompt));
            printf("%s", prompt);
            fflush(stdout);
        } else {
            // Vérifier s'il s'agit d'une réponse à une commande de salon
            if (response.type == REQ_MESSAGE && strcmp(response.sender, "Server") == 0) {
                // Détecter les messages de confirmation de salon
                if (strstr(response.content, "Vous avez rejoint le salon") != NULL) {
                    // Extraire le nom du salon de la réponse
                    char *start = strstr(response.content, "'");
                    if (start) {
                        start++; // Ignorer la première apostrophe
                        char *end = strstr(start, "'");
                        if (end) {
                            char room_name[50];
                            int len = end - start;
                            if (len > 0 && len < 49) {
                                strncpy(room_name, start, len);
                                room_name[len] = '\0';
                                update_current_room(client, room_name);
                            }
                        }
                    }
                } else if (strstr(response.content, "Vous avez quitté le salon") != NULL) {
                    update_current_room(client, "");  // Salon vide
                } else if (strstr(response.content, "créé avec succès") != NULL && 
                          strstr(response.content, "Salon") != NULL) {
                    // Extraire le nom du salon créé
                    char *start = strstr(response.content, "'");
                    if (start) {
                        start++;
                        char *end = strstr(start, "'");
                        if (end) {
                            char room_name[50];
                            int len = end - start;
                            if (len > 0 && len < 49) {
                                strncpy(room_name, start, len);
                                room_name[len] = '\0';
                                // Le créateur rejoint automatiquement son salon
                                update_current_room(client, room_name);
                            }
                        }
                    }
                } 
                // AJOUT: Détecter les informations utilisateur (@info)
                else if (strstr(response.content, "=== INFORMATIONS UTILISATEUR ===") != NULL) {
                    // Chercher l'information sur le salon courant
                    char *salon_line = strstr(response.content, "Salon courant: ");
                    if (salon_line) {
                        salon_line += 15; // Avancer après "Salon courant: "
                        
                        // Vérifier si le salon est "Aucun"
                        if (strncmp(salon_line, "Aucun", 5) == 0) {
                            // L'utilisateur n'est dans aucun salon
                            update_current_room(client, "");
                        } else {
                            // Extraire le nom du salon
                            char salon[50] = "";
                            int i = 0;
                            
                            // Copier jusqu'à rencontrer un '\n' ou un '\0'
                            while (salon_line[i] && salon_line[i] != '\n' && i < 49) {
                                salon[i] = salon_line[i];
                                i++;
                            }
                            salon[i] = '\0';
                            
                            // Mettre à jour le salon courant
                            update_current_room(client, salon);
                        }
                    }
                }
            }
            
            // Effacer la ligne actuelle avant d'afficher un nouveau message
            printf("\r                                                                               \r");
            
            // Traitement normal des messages
            printf("[%s] %s\n", response.sender, response.content);
            
            // Réafficher le prompt avec le salon courant
            char prompt[100];
            get_custom_prompt(client, prompt, sizeof(prompt));
            printf("%s", prompt);
            fflush(stdout);
        }

        if (strcmp(response.sender, "Server") == 0 && 
            strstr(response.content, "a quitté le chat") != NULL) {
            init_request(&ack_response, REQ_MESSAGE, client->username, 
                         "Server", "ACK notification déconnexion");
            send_request(client, &ack_response);
        }
    }
    
    printf("Thread de réception terminé.\n");
    return NULL;
}

// Fonction pour mettre à jour le salon courant
void update_current_room(Client *client, const char *room_name) {
    if (room_name == NULL || strlen(room_name) == 0) {
        client->current_room[0] = '\0';  // Salon vide
    } else {
        strncpy(client->current_room, room_name, sizeof(client->current_room) - 1);
        client->current_room[sizeof(client->current_room) - 1] = '\0';
    }
}

// Fonction pour obtenir le prompt personnalisé
void get_custom_prompt(Client *client, char *prompt, size_t size) {
    if (strlen(client->current_room) > 0) {
        snprintf(prompt, size, "[%s] %s: ", client->current_room, client->username);
    } else {
        snprintf(prompt, size, "[Hors salon] %s: ", client->username);
    }
}

// Fonction principale
int main(int argc, char *argv[]) {
    const char* server_ip;
    if(argc < 2){
        printf("Running on localhost (127.0.0.1)\n");
        server_ip = "127.0.0.1";
    }else if (argc == 2){
        server_ip = argv[1];
    }else {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    Client client;
    char username[50];
    char password[50];
    
    // Initialiser le client
    if (init_client(&client, server_ip) < 0) {
        return EXIT_FAILURE;
    }
    
    // Initialiser la clé pour stocker le client dans les threads
    if (pthread_key_create(&client_key, NULL) != 0) {
        perror("Erreur lors de la création de la clé thread");
        return EXIT_FAILURE;
    }
    
    // Demander les informations d'identification
    printf("Nom d'utilisateur: ");
    scanf("%49s", username);
    printf("Mot de passe: ");
    scanf("%49s", password);
    
    // Vider le buffer d'entrée
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
    
    // Se connecter au serveur
    if (connect_to_server(&client, username, password) < 0) {
        printf("\nÉchec de la connexion.\n");
        close(client.socket_fd);
        return EXIT_FAILURE;
    }
    
    printf("Connecté au serveur. Tapez vos messages (commandes préfixées par @).\n");
    
    // Créer les threads d'envoi et de réception
    pthread_t send_thread, receive_thread;
    
    if (pthread_create(&send_thread, NULL, send_message_thread, &client) != 0) {
        perror("Erreur lors de la création du thread d'envoi");
        close(client.socket_fd);
        return EXIT_FAILURE;
    }
    
    if (pthread_create(&receive_thread, NULL, receive_message_thread, &client) != 0) {
        perror("Erreur lors de la création du thread de réception");
        pthread_cancel(send_thread);
        close(client.socket_fd);
        return EXIT_FAILURE;
    }
    
    // Set global client for signal handler
    global_client = &client;
    
    // Install client-specific signal handler
    struct sigaction sa;
    sa.sa_handler = client_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    
    // Create pipe for signal handling
    if (pipe(signal_pipe) < 0) {
        perror("Erreur lors de la création du pipe de signal");
        return EXIT_FAILURE;
    }
    
    // Set stdin to non-blocking to avoid being stuck in fgets
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    // Attendre que les threads se terminent
    pthread_join(send_thread, NULL);

    if (running) {
        Request req;
        init_request(&req, REQ_DISCONNECT, client.username, "", "Déconnexion explicite");
        send_request(&client, &req);
        
        // Attendre un peu pour laisser le temps au serveur de traiter la déconnexion
        sleep(500000);  // 500ms
        
        // Arrêter le thread de réception
        running = 0;
    }


    pthread_join(receive_thread, NULL);
    
    // Fermer la socket
    close(client.socket_fd);
    
    // Nettoyage
    pthread_key_delete(client_key);
    
    printf("Client déconnecté.\n");
    
    return EXIT_SUCCESS;
}