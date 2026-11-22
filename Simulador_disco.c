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
static void imprimir_ubicacion(const Archivo *f, const char *prefijo)
{
    if (metodo == INDEXADA) {
        printf("%sbloques indice: ", prefijo);
        imprimir_rangos(f->index, f->n_index);
        printf("\n");
    }
    printf("%s%s: ", prefijo,
           metodo == ENLAZADA ? "bloques (orden de la cadena)" : "bloques de datos");
    imprimir_rangos(f->data, f->n_data);
    printf("\n");
}

static long calc_bloques_datos(long size_bytes)
{
    long util = (metodo == ENLAZADA) ? bloque_bytes - PTR_SIZE : bloque_bytes;
    return (size_bytes + util - 1) / util;
}

static long entradas_por_indice(void)
{
    return bloque_bytes / PTR_SIZE;
}

static long calc_bloques_indice(long n_data)
{
    long e = entradas_por_indice();
    long ni = (n_data - 1 + (e - 2)) / (e - 1);
    return ni < 1 ? 1 : ni;
}

static void calc_huecos(long *huecos, long *mayor)
{
    long run = 0;

    *huecos = 0;
    *mayor = 0;
    for (long i = 0; i < total_bloques; i++) {
        if (dueno[i] == -1) {
            run++;
        } else if (run) {
            (*huecos)++;
            if (run > *mayor) *mayor = run;
            run = 0;
        }
    }
    if (run) {
        (*huecos)++;
        if (run > *mayor) *mayor = run;
    }
}

static double pct_fragmentacion(long mayor)
{
    if (libres <= 0)
        return 0.0;
    return 100.0 * (1.0 - (double)mayor / (double)libres);
}

static void muestrear_fragmentacion(void)
{
    long huecos, mayor;
    double p;

    calc_huecos(&huecos, &mayor);
    p = pct_fragmentacion(mayor);
    frag_muestras++;
    frag_suma += p;
    if (p > frag_max)
        frag_max = p;
}

static long buscar_contiguo(long n)
{
    long run = 0;

    for (long i = 0; i < total_bloques; i++) {
        if (dueno[i] == -1) {
            if (++run == n)
                return i - n + 1;
        } else {
            run = 0;
        }
    }
    return -1;
}

static long siguiente_libre(long desde)
{
    while (dueno[desde] != -1)
        desde++;
    return desde;
}

static int crear_archivo(long size_bytes, char *motivo, size_t mlen)
{
    long n_data = calc_bloques_datos(size_bytes);
    long n_idx  = (metodo == INDEXADA) ? calc_bloques_indice(n_data) : 0;
    long total  = n_data + n_idx;
    long inicio = -1;
    Archivo *f;

    if (total > libres) {
        snprintf(motivo, mlen,
                 "espacio insuficiente: el archivo requiere %ld bloques y quedan %ld libres",
                 total, libres);
        return 0;
    }

    if (metodo == CONTIGUA) {
        inicio = buscar_contiguo(total);
        if (inicio < 0) {
            long huecos, mayor;
            calc_huecos(&huecos, &mayor);
            snprintf(motivo, mlen,
                     "fragmentacion externa: se requieren %ld bloques contiguos, "
                     "pero el mayor hueco es de %ld (libres en total: %ld, huecos: %ld)",
                     total, mayor, libres, huecos);
            return 0;
        }
    }

    if (n_archivos == cap_archivos) {
        cap_archivos = cap_archivos ? cap_archivos * 2 : 64;
        archivos = realloc(archivos, (size_t)cap_archivos * sizeof(Archivo));
        if (!archivos) {
            perror("realloc");
            exit(1);
        }
    }

    f = &archivos[n_archivos];
    memset(f, 0, sizeof *f);
    f->id = n_archivos + 1;
    f->size_bytes = size_bytes;
    f->activo = 1;
    f->n_data = (int)n_data;
    f->n_index = (int)n_idx;
    f->data = malloc((size_t)n_data * sizeof(int));
    if (n_idx)
        f->index = malloc((size_t)n_idx * sizeof(int));
    if (!f->data || (n_idx && !f->index)) {
        perror("malloc");
        exit(1);
    }

    if (metodo == CONTIGUA) {
        for (long i = 0; i < n_data; i++) {
            f->data[i] = (int)(inicio + i);
            dueno[inicio + i] = f->id;
        }
        f->first = (int)inicio;
    } else {
        long cursor = 0;

        for (long i = 0; i < n_idx; i++) {
            cursor = siguiente_libre(cursor);
            f->index[i] = (int)cursor;
            dueno[cursor] = f->id;
            cursor++;
        }
        for (long i = 0; i < n_data; i++) {
            cursor = siguiente_libre(cursor);
            f->data[i] = (int)cursor;
            dueno[cursor] = f->id;
            cursor++;
        }
        if (metodo == ENLAZADA) {
            for (long i = 0; i < n_data; i++)
                sig[f->data[i]] = (i + 1 < n_data) ? f->data[i + 1] : -1;
            f->first = f->data[0];
        } else {
            f->first = f->index[0];
        }
    }

    libres -= total;
    n_activos++;
    n_archivos++;
    creados++;

    printf("[t=%3lds] CREAR    archivo #%d  tamano=%ld KB  bloques=%ld",
           tiempo, f->id, (size_bytes + 1023) / 1024, total);
    if (metodo == INDEXADA)
        printf(" (datos=%ld, indice=%ld)", n_data, n_idx);
    else if (metodo == ENLAZADA)
        printf(" (cada bloque guarda un puntero de %d bytes)", PTR_SIZE);
    printf("\n");
    imprimir_ubicacion(f, "           ");
    return 1;
}