// client.c
#include "client.h"

int init_client(Client *client, const char *server_ip) {
    // Créer la socket UDP
    client->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client->socket_fd < 0) {
        perror("Erreur lors de la création de la socket");
        return -1;
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
    
    // Configuration du gestionnaire de signal pour CTRL+C
    signal(SIGINT, handle_sigint);
    
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

void *send_message_thread(void *arg) {
    Client *client = (Client *)arg;
    char buffer[MAX_MSG_SIZE];
    Request req;
    
    // Boucle pour envoyer des messages
    while (running) {
        // Lire l'entrée utilisateur
        if (fgets(buffer, MAX_MSG_SIZE, stdin) == NULL) {
            continue;
        }
        
        // Supprimer le caractère de nouvelle ligne
        buffer[strcspn(buffer, "\n")] = '\0';
        
        // Vérifier si c'est une commande ou un message
        if (buffer[0] == '@') {
            // C'est une commande
            init_request(&req, REQ_COMMAND, client->username, "", buffer);
        } else {
            // C'est un message normal
            init_request(&req, REQ_MESSAGE, client->username, "", buffer);
        }
        
        // Envoyer la requête
        send_request(client, &req);
    }
    
    // Envoyer un message de déconnexion avant de quitter
    init_request(&req, REQ_DISCONNECT, client->username, "", "Déconnexion");
    send_request(client, &req);
    
    return NULL;
}

void *receive_message_thread(void *arg) {
    Client *client = (Client *)arg;
    Request response;
    socklen_t server_len = sizeof(client->server_addr);
    
    // Boucle pour recevoir des messages
    while (running) {
        // Recevoir une réponse du serveur
        ssize_t received = recvfrom(client->socket_fd, &response, sizeof(Request), 0,
                                   (struct sockaddr*)&client->server_addr, &server_len);
        
        if (received < 0) {
            if (running) {
                perror("Erreur lors de la réception de la réponse");
            }
            continue;
        }
        
        // Afficher le message reçu
        printf("[%s] %s\n", response.sender, response.content);
    }
    
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
    pthread_join(receive_thread, NULL);
    
    // Fermer la socket
    close(client.socket_fd);
    
    printf("Client déconnecté.\n");
    
    return EXIT_SUCCESS;
}