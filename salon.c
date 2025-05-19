#define MAX_SALONS 32
#define MAX_MEMBRES 32
#define MAX_NOM_SALON 50

typedef struct {
    char nom[MAX_NOM_SALON];
    char membres[MAX_MEMBRES][50]; // pseudos
    int  nb_membres;
} Salon;
