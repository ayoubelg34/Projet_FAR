// common.c
#include "common.h"

volatile sig_atomic_t running = 1;
int global_socket_fd = -1;  // Initialisation de la variable globale

void init_request(Request *req, RequestType type, const char *sender, 
                  const char *recipient, const char *content) {
    req->type = type;
    
    strncpy(req->sender, sender, sizeof(req->sender) - 1);
    req->sender[sizeof(req->sender) - 1] = '\0';
    
    strncpy(req->recipient, recipient, sizeof(req->recipient) - 1);
    req->recipient[sizeof(req->recipient) - 1] = '\0';
    
    strncpy(req->content, content, sizeof(req->content) - 1);
    req->content[sizeof(req->content) - 1] = '\0';
}

void handle_sigint(int sig) {
    printf("\nInterruption reçue (signal %d). Arrêt en cours...\n", sig);
    // Just set the running flag to 0, don't close the socket here
    // This will allow the main thread to send shutdown messages before closing
    running = 0;
}

// Génère un nom de fichier unique quand un fichier du même nom existe déjà
char* generate_unique_filename(const char *dir, const char *original_filename, char *buffer, size_t buffer_size) {
    // Extraire le nom de base et l'extension
    char basename_buf[256];
    char extension[64] = "";
    
    // Copier le nom original
    strncpy(basename_buf, original_filename, sizeof(basename_buf) - 1);
    basename_buf[sizeof(basename_buf) - 1] = '\0';
    
    // Chercher l'extension
    char *dot = strrchr(basename_buf, '.');
    if (dot) {
        strcpy(extension, dot); // Copier l'extension avec le point
        *dot = '\0'; // Tronquer le nom de base
    }
    
    // Construire le chemin complet pour tester
    char test_path[512];
    snprintf(test_path, sizeof(test_path), "%s/%s%s", dir, basename_buf, extension);
    
    // Si le fichier n'existe pas, retourner le nom original
    if (access(test_path, F_OK) == -1) {
        snprintf(buffer, buffer_size, "%s%s", basename_buf, extension);
        return buffer;
    }
    
    // Sinon, essayer avec "_copy", "_copy2", etc.
    int copy_num = 1;
    do {
        if (copy_num == 1) {
            snprintf(buffer, buffer_size, "%s_copy%s", basename_buf, extension);
        } else {
            snprintf(buffer, buffer_size, "%s_copy%d%s", basename_buf, copy_num, extension);
        }
        
        snprintf(test_path, sizeof(test_path), "%s/%s", dir, buffer);
        copy_num++;
    } while (access(test_path, F_OK) != -1 && copy_num < 100); // Limitation à 100 copies
    
    return buffer;
}

// Fonction pour recevoir un fichier via TCP
int receive_file_tcp(const char *save_dir, const char *remote_ip, int port, int mode) {
    int tcp_socket = -1;  // Initialisation à une valeur invalide
    int client_socket = -1; // Initialisation explicite
    struct sockaddr_in remote_addr;
    FILE *file = NULL;
    char buffer[1024];
    ssize_t bytes_received;
    
    // Créer une socket TCP
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("Erreur lors de la création de la socket TCP");
        return -1;
    }
    
    // En mode client, nous voulons nous connecter au serveur
    if (mode == 0) {
        // Configurer l'adresse du serveur
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, remote_ip, &remote_addr.sin_addr) <= 0) {
            perror("Adresse IP invalide");
            close(tcp_socket);
            return -1;
        }
        
        // Se connecter au serveur
        if (connect(tcp_socket, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
            perror("Erreur lors de la connexion au serveur");
            close(tcp_socket);
            return -1;
        }
        
        client_socket = tcp_socket; // En mode client, socket client = socket TCP
    }
    // En mode serveur, nous acceptons une connexion entrante
    else if (mode == 1) {
        // Code pour accepter une connexion
        // ...
        // client_socket est initialisé ici
    }
    
    // Vérifier que nous avons une socket client valide
    if (client_socket < 0) {
        perror("Socket client invalide");
        if (tcp_socket >= 0)
            close(tcp_socket);
        return -1;
    }
    
    // Recevoir le nom du fichier
    char filename[256];
    if ((bytes_received = recv(client_socket, filename, sizeof(filename), 0)) <= 0) {
        perror("Erreur lors de la réception du nom de fichier");
        if (mode == 1 && client_socket != tcp_socket) 
            close(client_socket);
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
    if (send(client_socket, "OK", 3, 0) < 0) {
        perror("Erreur lors de l'envoi de l'ACK");
        if (mode == 1 && client_socket != tcp_socket) 
            close(client_socket);
        close(tcp_socket);
        return -1;
    }
    
    // S'assurer que le répertoire existe
    mkdir(save_dir, 0755);
    
    // Construire le chemin complet du fichier avec le nom unique
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", save_dir, unique_filename);
    
    // Ouvrir le fichier pour écriture
    file = fopen(filepath, "wb");
    if (file == NULL) {
        perror("Erreur lors de la création du fichier");
        if (mode == 1 && client_socket != tcp_socket) 
            close(client_socket);
        close(tcp_socket);
        return -1;
    }
    
    // Recevoir et écrire le contenu du fichier
    while ((bytes_received = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        if (fwrite(buffer, 1, (size_t)bytes_received, file) != (size_t)bytes_received) {
            perror("Erreur lors de l'écriture dans le fichier");
            fclose(file);
            if (mode == 1 && client_socket != tcp_socket) 
                close(client_socket);
            close(tcp_socket);
            return -1;
        }
    }
    
    if (bytes_received < 0) {
        perror("Erreur lors de la réception du fichier");
        fclose(file);
        if (mode == 1 && client_socket != tcp_socket) 
            close(client_socket);
        close(tcp_socket);
        return -1;
    }
    
    printf("Fichier reçu avec succès: %s\n", filepath);
    
    // Fermer le fichier et les sockets
    fclose(file);
    if (mode == 1 && client_socket != tcp_socket) 
        close(client_socket);
    close(tcp_socket);
    
    return 0;
}