#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define PTR_SIZE         4
#define MAX_RANGOS       40
#define PERIODO_CREAR    2
#define INICIO_EVENTOS   30
#define PERIODO_EVENTOS  5

typedef enum { CONTIGUA, ENLAZADA, INDEXADA } Metodo;

typedef struct {
    int   id;
    long  size_bytes;
    int   activo;
    int   n_data;
    int  *data;      
    int   n_index;
    int  *index;     
    int   first;     
} Archivo;

static Metodo metodo;
static long   disco_mb, bloque_kb, bloque_bytes, total_bloques, libres;
static long   min_kb, max_kb;

static int      *dueno;     
static int      *sig;       
static Archivo  *archivos;  
static int       n_archivos = 0, cap_archivos = 0, n_activos = 0;

static long creados = 0, eliminados = 0;
static long n_accesos_consulta = 0, suma_accesos = 0;
static long frag_muestras = 0;
static double frag_suma = 0.0, frag_max = 0.0;
static long tiempo = 0;

static const char *nombre_metodo(void)
{
    switch (metodo) {
    case CONTIGUA: return "contigua";
    case ENLAZADA: return "enlazada";
    default:       return "indexada";
    }
}

static uint64_t rand64(void)
{
    return ((uint64_t)random() << 31) ^ (uint64_t)random();
}

static long rand_rango(long a, long b)
{
    return a + (long)(rand64() % (uint64_t)(b - a + 1));
}

static long leer_long(const char *msg, long min, long max)
{
    char buf[128];
    char *fin;
    long v;

    for (;;) {
        printf("%s", msg);
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) {
            fprintf(stderr, "\nEntrada cerrada.\n");
            exit(1);
        }
        v = strtol(buf, &fin, 10);
        if (fin == buf || (*fin != '\n' && *fin != '\0')) {
            printf("  Valor invalido.\n");
            continue;
        }
        if (v < min || v > max) {
            printf("  Debe estar entre %ld y %ld.\n", min, max);
            continue;
        }
        return v;
    }
}

static Metodo leer_metodo(void)
{
    char buf[64];

    for (;;) {
        printf("Mecanismo de asignacion (contigua / enlazada / indexada): ");
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) {
            fprintf(stderr, "\nEntrada cerrada.\n");
            exit(1);
        }
        buf[strcspn(buf, " \t\r\n")] = '\0';
        if (!strcasecmp(buf, "contigua")) return CONTIGUA;
        if (!strcasecmp(buf, "enlazada")) return ENLAZADA;
        if (!strcasecmp(buf, "indexada")) return INDEXADA;
        printf("  Opcion invalida.\n");
    }
}

static void imprimir_rangos(const int *b, int n)
{
    int i = 0, rangos = 0;

    if (n == 0) {
        printf("(ninguno)");
        return;
    }
    while (i < n) {
        int j = i;
        while (j + 1 < n && b[j + 1] == b[j] + 1)
            j++;
        if (rangos >= MAX_RANGOS) {
            printf(", ...");
            return;
        }
        if (rangos)
            printf(", ");
        if (j == i)
            printf("%d", b[i]);
        else
            printf("%d-%d", b[i], b[j]);
        rangos++;
        i = j + 1;
    }
}