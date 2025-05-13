// client.c
#include "client.h"
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>

// External variables defined in common.c
extern volatile sig_atomic_t running;
extern int global_socket_fd;

// Signal handler is defined in common.c

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
    return send_request(client, &req);
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

void *send_message_thread(void *arg) {
    Client *client = (Client *)arg;
    char buffer[MAX_MSG_SIZE];
    Request req;
    
    // Boucle pour envoyer des messages
    while (running) {
        // Prompt utilisateur
        printf("Vous: ");
        fflush(stdout);
        
        // Lire l'entrée utilisateur
        if (fgets(buffer, MAX_MSG_SIZE, stdin) == NULL) {
            if (!running) {  // Si interruption causée par le signal
                break;
            }
            continue;
        }
        
        // Supprimer le caractère de nouvelle ligne
        buffer[strcspn(buffer, "\n")] = '\0';
        
        // Vérifier si c'est une commande d'upload de fichier
        if (strncmp(buffer, "@upload ", 8) == 0) {
            // Extraire le nom du fichier
            char *filename = buffer + 8;
            
            // Informer l'utilisateur
            printf("Envoi du fichier %s en cours...\n", filename);
            
            // Envoyer le fichier
            if (send_file(filename, inet_ntoa(client->server_addr.sin_addr)) == 0) {
                // Informer le serveur de l'envoi réussi via UDP
                char notification[MAX_MSG_SIZE];
                snprintf(notification, sizeof(notification), "@file_uploaded %s", basename(filename));
                init_request(&req, REQ_COMMAND, client->username, "", notification);
                send_request(client, &req);
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
        } else {
            // C'est un message normal
            init_request(&req, REQ_MESSAGE, client->username, "", buffer);
            send_request(client, &req);
        }
    }
    
    // Envoyer un message de déconnexion avant de quitter
    init_request(&req, REQ_DISCONNECT, client->username, "", "Déconnexion");
    send_request(client, &req);
    
    return NULL;
}

void *receive_message_thread(void *arg) {
    Client *client = (Client *)arg;
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
            printf("\rNotification: Fichier prêt à être téléchargé. Lancement du téléchargement...\n");
            
            // Télécharger le fichier
            char download_dir[256] = "./downloads"; // Dossier par défaut
            
            // Créer le dossier s'il n'existe pas
            mkdir(download_dir, 0755);
            
            if (receive_file(download_dir, inet_ntoa(client->server_addr.sin_addr)) == 0) {
                printf("Fichier téléchargé avec succès dans %s\n", download_dir);
            }
            
            printf("Vous: ");
            fflush(stdout);
        } else {
            // Traitement normal des messages
            printf("\r[%s] %s\n", response.sender, response.content);
            printf("Vous: ");
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

// Fonction principale
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    Client client;
    char username[50];
    char password[50];
    
    // Initialiser le client
    if (init_client(&client, argv[1]) < 0) {
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
    
    // Attendre que les threads se terminent
    pthread_join(send_thread, NULL);

    if (running) {
        Request req;
        init_request(&req, REQ_DISCONNECT, client.username, "", "Déconnexion explicite");
        send_request(&client, &req);
        
        // Attendre un peu pour laisser le temps au serveur de traiter la déconnexion
        usleep(500000);  // 500ms
        
        // Arrêter le thread de réception
        running = 0;
    }


    pthread_join(receive_thread, NULL);
    
    // Fermer la socket
    close(client.socket_fd);
    
    printf("Client déconnecté.\n");
    
    return EXIT_SUCCESS;
}