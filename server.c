/*
 * =============================================================================
 *  server.c — Servidor Central de Monitoreo de Sensores IoT
 * =============================================================================
 *
 *  Compilar:  gcc -o server server.c -lpthread
 *  Ejecutar:  ./server <puerto_tcp> <puerto_http> <archivoDeLogs>
 *
 *  Protocolo basado en texto (mensajes delimitados por '\n').
 *  Concurrencia: un hilo POSIX por cada cliente conectado.
 *  Estructuras compartidas protegidas con mutex globales.
 *
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <time.h>
#include <errno.h>

/* ========================== CONSTANTES ====================================*/
#define MAX_SENSORS    100
#define MAX_OPERATORS   50
#define HISTORY_SIZE    10
#define BUFFER_SIZE   4096
#define MAX_TOKENS      16

/* Umbrales de alerta */
#define THRESHOLD_TEMPERATURE  80.0f
#define THRESHOLD_VIBRATION    10.0f
#define THRESHOLD_ENERGY      500.0f

/* ========================== ESTRUCTURAS ===================================*/

/* Información de un sensor registrado */
typedef struct {
    char  id[32];
    char  type[32];          /* TEMPERATURE, VIBRATION, ENERGY               */
    char  status[16];        /* ACTIVE, INACTIVE                             */
    float last_value;
    long  last_timestamp;
    float history[HISTORY_SIZE];
    int   history_count;     /* total de mediciones insertadas (módulo 10)   */
    int   client_fd;         /* fd del socket del sensor (-1 si inactivo)    */
} sensor_t;

/* Información de un operador conectado */
typedef struct {
    char username[32];
    int  client_fd;
    char ip[INET6_ADDRSTRLEN];
    int  port;
} operator_t;

/* Información que se pasa al hilo de cada cliente */
typedef struct {
    int  fd;
    char ip[INET6_ADDRSTRLEN];
    int  port;
} client_info_t;

/* ========================== VARIABLES GLOBALES ============================*/

/* Listas compartidas */
static sensor_t   sensors[MAX_SENSORS];
static int        sensor_count = 0;

static operator_t operators[MAX_OPERATORS];
static int        operator_count = 0;

/* Mutexes para proteger las estructuras compartidas */
static pthread_mutex_t sensor_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t operator_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex      = PTHREAD_MUTEX_INITIALIZER;

/* Archivo de log (abierto en modo append) */
static FILE *log_file = NULL;

/* ========================== FUNCIONES AUXILIARES ===========================*/

/*
 * get_timestamp_str() — devuelve una cadena con la fecha/hora actual
 * en formato legible para los logs.
 */
static void get_timestamp_str(char *buf, size_t len)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
}

/*
 * send_response() — envía una cadena al cliente asegurándose de que
 * termina en '\n'.  Retorna 0 si fue exitoso, -1 si falló.
 */
static int send_response(int fd, const char *msg)
{
    size_t len = strlen(msg);
    /* Verificar si ya termina en \n */
    if (len == 0) return 0;

    if (send(fd, msg, len, MSG_NOSIGNAL) < 0)
        return -1;

    /* Si el mensaje no termina en \n, agregar uno */
    if (msg[len - 1] != '\n') {
        if (send(fd, "\n", 1, MSG_NOSIGNAL) < 0)
            return -1;
    }
    return 0;
}

/* ========================== LOGGING =======================================*/

/*
 * log_event() — registra un evento en consola y en el archivo de log.
 * Protegido con mutex para evitar interleaving entre hilos.
 *
 * Formato: [TIMESTAMP] [IP:PUERTO] [RECIBIDO: msg] [ENVIADO: resp]
 */
void log_event(char *ip, int port, char *received, char *sent)
{
    char ts[64];
    get_timestamp_str(ts, sizeof(ts));

    /* Copiar las cadenas para poder limpiar saltos de línea */
    char recv_clean[BUFFER_SIZE];
    char sent_clean[BUFFER_SIZE];
    strncpy(recv_clean, received ? received : "(none)", sizeof(recv_clean) - 1);
    recv_clean[sizeof(recv_clean) - 1] = '\0';
    strncpy(sent_clean, sent ? sent : "(none)", sizeof(sent_clean) - 1);
    sent_clean[sizeof(sent_clean) - 1] = '\0';

    /* Eliminar '\n' finales para que el log sea de una sola línea */
    size_t rlen = strlen(recv_clean);
    if (rlen > 0 && recv_clean[rlen - 1] == '\n') recv_clean[rlen - 1] = '\0';
    size_t slen = strlen(sent_clean);
    if (slen > 0 && sent_clean[slen - 1] == '\n') sent_clean[slen - 1] = '\0';

    pthread_mutex_lock(&log_mutex);

    /* Consola */
    printf("[%s] [%s:%d] [RECIBIDO: %s] [ENVIADO: %s]\n",
           ts, ip, port, recv_clean, sent_clean);
    fflush(stdout);

    /* Archivo */
    if (log_file) {
        fprintf(log_file, "[%s] [%s:%d] [RECIBIDO: %s] [ENVIADO: %s]\n",
                ts, ip, port, recv_clean, sent_clean);
        fflush(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}

/* ========================== BROADCAST A OPERADORES ========================*/

/*
 * broadcast_to_operators() — envía un mensaje a TODOS los operadores
 * conectados.  Se usa para alertas y notificaciones SENSOR_DOWN.
 */
void broadcast_to_operators(char *message)
{
    pthread_mutex_lock(&operator_mutex);
    for (int i = 0; i < operator_count; i++) {
        /* Intentar enviar; si falla, ignorar (el hilo del operador
         * detectará la desconexión más tarde) */
        send_response(operators[i].client_fd, message);
    }
    pthread_mutex_unlock(&operator_mutex);
}

/* ========================== VERIFICACIÓN DE ALERTAS =======================*/

/*
 * check_alert() — compara el valor recibido con los umbrales definidos
 * y, si se excede, construye un mensaje ALERT y lo envía a los operadores.
 */
void check_alert(sensor_t *sensor, float value)
{
    char alert_msg[256];
    int  alert = 0;

    if (strcmp(sensor->type, "TEMPERATURE") == 0 && value > THRESHOLD_TEMPERATURE) {
        snprintf(alert_msg, sizeof(alert_msg),
                 "ALERT %s HIGH_TEMP %.2f\n", sensor->id, value);
        alert = 1;
    } else if (strcmp(sensor->type, "VIBRATION") == 0 && value > THRESHOLD_VIBRATION) {
        snprintf(alert_msg, sizeof(alert_msg),
                 "ALERT %s HIGH_VIBRATION %.2f\n", sensor->id, value);
        alert = 1;
    } else if (strcmp(sensor->type, "ENERGY") == 0 && value > THRESHOLD_ENERGY) {
        snprintf(alert_msg, sizeof(alert_msg),
                 "ALERT %s HIGH_ENERGY %.2f\n", sensor->id, value);
        alert = 1;
    }

    if (alert) {
        broadcast_to_operators(alert_msg);
    }
}

/* ========================== BÚSQUEDA DE SENSOR ============================*/

/*
 * find_sensor() — busca un sensor por id.
 * Debe llamarse con sensor_mutex ya tomado.
 * Retorna el índice en el array, o -1 si no existe.
 */
static int find_sensor(const char *id)
{
    for (int i = 0; i < sensor_count; i++) {
        if (strcmp(sensors[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* ========================== TOKENIZER =====================================*/

/*
 * tokenize() — divide una cadena en tokens separados por espacios.
 * Modifica la cadena original.  Retorna la cantidad de tokens.
 */
static int tokenize(char *line, char **tokens, int max_tokens)
{
    int count = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok && count < max_tokens) {
        tokens[count++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    return count;
}

/* ========================== HANDLERS DE PROTOCOLO =========================*/

/*
 * handle_register_sensor() — procesa "REGISTER SENSOR <id> <tipo>"
 *   tokens[0] = "REGISTER"
 *   tokens[1] = "SENSOR"
 *   tokens[2] = <id>
 *   tokens[3] = <tipo>
 */
static void handle_register_sensor(int fd, char **tokens, int ntokens,
                                   client_info_t *info, char *raw_msg)
{
    char response[256];

    if (ntokens < 4) {
        snprintf(response, sizeof(response), "ERROR 400 BAD_REQUEST\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    const char *id   = tokens[2];
    const char *type = tokens[3];

    /* Validar tipo */
    if (strcmp(type, "TEMPERATURE") != 0 &&
        strcmp(type, "VIBRATION")   != 0 &&
        strcmp(type, "ENERGY")      != 0) {
        snprintf(response, sizeof(response), "ERROR 400 INVALID_SENSOR_TYPE\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    pthread_mutex_lock(&sensor_mutex);

    /* Verificar si ya existe */
    int idx = find_sensor(id);
    if (idx >= 0) {
        /* Si está inactivo, reactivar con el nuevo fd */
        if (strcmp(sensors[idx].status, "INACTIVE") == 0) {
            strcpy(sensors[idx].status, "ACTIVE");
            sensors[idx].client_fd = fd;
            pthread_mutex_unlock(&sensor_mutex);
            snprintf(response, sizeof(response), "OK REGISTERED %s\n", id);
            send_response(fd, response);
            log_event(info->ip, info->port, raw_msg, response);
            return;
        }
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 409 SENSOR_ALREADY_REGISTERED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    /* Verificar capacidad */
    if (sensor_count >= MAX_SENSORS) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 500 MAX_SENSORS_REACHED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    /* Registrar nuevo sensor */
    sensor_t *s = &sensors[sensor_count];
    strncpy(s->id, id, sizeof(s->id) - 1);
    s->id[sizeof(s->id) - 1] = '\0';
    strncpy(s->type, type, sizeof(s->type) - 1);
    s->type[sizeof(s->type) - 1] = '\0';
    strcpy(s->status, "ACTIVE");
    s->last_value     = 0.0f;
    s->last_timestamp  = 0;
    s->history_count   = 0;
    s->client_fd       = fd;
    memset(s->history, 0, sizeof(s->history));
    sensor_count++;

    pthread_mutex_unlock(&sensor_mutex);

    snprintf(response, sizeof(response), "OK REGISTERED %s\n", id);
    send_response(fd, response);
    log_event(info->ip, info->port, raw_msg, response);
}

/*
 * handle_sensor_data() — procesa "DATA <id> <valor> <timestamp>"
 */
void handle_sensor_data(int fd, char **tokens, int ntokens,
                        client_info_t *info, char *raw_msg)
{
    char response[256];

    if (ntokens < 4) {
        snprintf(response, sizeof(response), "ERROR 400 BAD_REQUEST\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    const char *id        = tokens[1];
    float       value     = atof(tokens[2]);
    long        timestamp = atol(tokens[3]);

    pthread_mutex_lock(&sensor_mutex);

    int idx = find_sensor(id);
    if (idx < 0) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 404 SENSOR_NOT_FOUND\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    sensor_t *s = &sensors[idx];

    /* Verificar que el sensor está activo y pertenece a este cliente */
    if (strcmp(s->status, "ACTIVE") != 0 || s->client_fd != fd) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 401 UNAUTHORIZED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    /* Actualizar datos del sensor */
    s->last_value    = value;
    s->last_timestamp = timestamp;

    /* Almacenar en historial circular */
    int hist_idx = s->history_count % HISTORY_SIZE;
    s->history[hist_idx] = value;
    s->history_count++;

    /* Verificar umbrales (dentro del lock para leer el tipo) */
    /* Copiamos lo necesario para llamar check_alert fuera del lock */
    sensor_t sensor_copy = *s;

    pthread_mutex_unlock(&sensor_mutex);

    snprintf(response, sizeof(response), "OK DATA RECEIVED\n");
    send_response(fd, response);
    log_event(info->ip, info->port, raw_msg, response);

    /* Verificar si se dispara una alerta */
    check_alert(&sensor_copy, value);
}

/*
 * handle_disconnect() — procesa "DISCONNECT <id>"
 * Desconexión limpia de un sensor.
 */
static void handle_disconnect(int fd, char **tokens, int ntokens,
                              client_info_t *info, char *raw_msg)
{
    char response[256];

    if (ntokens < 2) {
        snprintf(response, sizeof(response), "ERROR 400 BAD_REQUEST\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    const char *id = tokens[1];

    pthread_mutex_lock(&sensor_mutex);

    int idx = find_sensor(id);
    if (idx < 0) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 404 SENSOR_NOT_FOUND\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    sensor_t *s = &sensors[idx];

    /* Verificar que el sensor pertenece a este cliente */
    if (s->client_fd != fd) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 401 UNAUTHORIZED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    strcpy(s->status, "INACTIVE");
    s->client_fd = -1;

    pthread_mutex_unlock(&sensor_mutex);

    snprintf(response, sizeof(response), "OK DISCONNECTED %s\n", id);
    send_response(fd, response);
    log_event(info->ip, info->port, raw_msg, response);
}

/*
 * handle_register_operator() — procesa "REGISTER OPERATOR <usuario>"
 */
static void handle_register_operator(int fd, char **tokens, int ntokens,
                                     client_info_t *info, char *raw_msg)
{
    char response[256];

    if (ntokens < 3) {
        snprintf(response, sizeof(response), "ERROR 400 BAD_REQUEST\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    const char *username = tokens[2];

    pthread_mutex_lock(&operator_mutex);

    /* Verificar si el operador ya existe */
    for (int i = 0; i < operator_count; i++) {
        if (strcmp(operators[i].username, username) == 0) {
            pthread_mutex_unlock(&operator_mutex);
            snprintf(response, sizeof(response),
                     "ERROR 409 OPERATOR_ALREADY_REGISTERED\n");
            send_response(fd, response);
            log_event(info->ip, info->port, raw_msg, response);
            return;
        }
    }

    /* Verificar capacidad */
    if (operator_count >= MAX_OPERATORS) {
        pthread_mutex_unlock(&operator_mutex);
        snprintf(response, sizeof(response), "ERROR 500 MAX_OPERATORS_REACHED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    /* Registrar nuevo operador */
    operator_t *op = &operators[operator_count];
    strncpy(op->username, username, sizeof(op->username) - 1);
    op->username[sizeof(op->username) - 1] = '\0';
    op->client_fd = fd;
    strncpy(op->ip, info->ip, sizeof(op->ip) - 1);
    op->ip[sizeof(op->ip) - 1] = '\0';
    op->port = info->port;
    operator_count++;

    pthread_mutex_unlock(&operator_mutex);

    snprintf(response, sizeof(response), "OK REGISTERED %s\n", username);
    send_response(fd, response);
    log_event(info->ip, info->port, raw_msg, response);
}

/*
 * handle_list_sensors() — procesa "LIST SENSORS"
 * Responde con la lista de sensores en formato:
 *   SENSORS id1:tipo1:estado1 id2:tipo2:estado2 ...
 */
static void handle_list_sensors(int fd, client_info_t *info, char *raw_msg)
{
    char response[BUFFER_SIZE];
    int  offset = 0;

    pthread_mutex_lock(&sensor_mutex);

    offset += snprintf(response + offset, sizeof(response) - offset, "SENSORS");

    if (sensor_count == 0) {
        offset += snprintf(response + offset, sizeof(response) - offset, " (none)");
    } else {
        for (int i = 0; i < sensor_count; i++) {
            offset += snprintf(response + offset, sizeof(response) - offset,
                               " %s:%s:%s",
                               sensors[i].id,
                               sensors[i].type,
                               sensors[i].status);
        }
    }

    snprintf(response + offset, sizeof(response) - offset, "\n");

    pthread_mutex_unlock(&sensor_mutex);

    send_response(fd, response);
    log_event(info->ip, info->port, raw_msg, response);
}

/*
 * handle_get_data() — procesa "GET DATA <id>"
 * Responde con las últimas mediciones almacenadas del sensor.
 */
static void handle_get_data(int fd, char **tokens, int ntokens,
                            client_info_t *info, char *raw_msg)
{
    char response[BUFFER_SIZE];

    if (ntokens < 3) {
        snprintf(response, sizeof(response), "ERROR 400 BAD_REQUEST\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    const char *id = tokens[2];

    pthread_mutex_lock(&sensor_mutex);

    int idx = find_sensor(id);
    if (idx < 0) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 404 SENSOR_NOT_FOUND\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    sensor_t *s = &sensors[idx];

    /* Determinar cuántas mediciones válidas hay */
    int total = s->history_count;
    int count = (total < HISTORY_SIZE) ? total : HISTORY_SIZE;

    if (count == 0) {
        snprintf(response, sizeof(response),
                 "DATA_RESPONSE %s NO_DATA %ld\n", id, s->last_timestamp);
        pthread_mutex_unlock(&sensor_mutex);
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    /*
     * Enviar cada medición como una línea DATA_RESPONSE.
     * Recorremos el historial circular desde la más antigua a la más reciente.
     */
    int start;
    if (total <= HISTORY_SIZE) {
        start = 0;
    } else {
        start = total % HISTORY_SIZE;  /* la más antigua */
    }

    /* Construimos todas las respuestas juntas */
    char all_responses[BUFFER_SIZE];
    int  off = 0;

    for (int i = 0; i < count; i++) {
        int hi = (start + i) % HISTORY_SIZE;
        off += snprintf(all_responses + off, sizeof(all_responses) - off,
                        "DATA_RESPONSE %s %.2f %ld\n",
                        id, s->history[hi], s->last_timestamp);
    }

    pthread_mutex_unlock(&sensor_mutex);

    /* Enviar todo de una vez */
    send(fd, all_responses, strlen(all_responses), MSG_NOSIGNAL);
    log_event(info->ip, info->port, raw_msg, all_responses);
}

/* ========================== PARSER Y DISPATCHER ===========================*/

/*
 * parse_and_dispatch() — analiza el mensaje recibido, lo tokeniza
 * y lo despacha al handler correspondiente.
 */
void parse_and_dispatch(int client_fd, char *message, client_info_t *info)
{
    /* Guardar una copia del mensaje original para logging */
    char raw_msg[BUFFER_SIZE];
    strncpy(raw_msg, message, sizeof(raw_msg) - 1);
    raw_msg[sizeof(raw_msg) - 1] = '\0';

    /* Tokenizar */
    char *tokens[MAX_TOKENS];
    int   ntokens = tokenize(message, tokens, MAX_TOKENS);

    if (ntokens == 0) {
        char resp[] = "ERROR 400 BAD_REQUEST\n";
        send_response(client_fd, resp);
        log_event(info->ip, info->port, raw_msg, resp);
        return;
    }

    /* ---- REGISTER SENSOR / REGISTER OPERATOR ---- */
    if (strcmp(tokens[0], "REGISTER") == 0 && ntokens >= 2) {
        if (strcmp(tokens[1], "SENSOR") == 0) {
            handle_register_sensor(client_fd, tokens, ntokens, info, raw_msg);
            return;
        }
        if (strcmp(tokens[1], "OPERATOR") == 0) {
            handle_register_operator(client_fd, tokens, ntokens, info, raw_msg);
            return;
        }
    }

    /* ---- DATA ---- */
    if (strcmp(tokens[0], "DATA") == 0) {
        handle_sensor_data(client_fd, tokens, ntokens, info, raw_msg);
        return;
    }

    /* ---- DISCONNECT ---- */
    if (strcmp(tokens[0], "DISCONNECT") == 0) {
        handle_disconnect(client_fd, tokens, ntokens, info, raw_msg);
        return;
    }

    /* ---- LIST SENSORS ---- */
    if (strcmp(tokens[0], "LIST") == 0 && ntokens >= 2 &&
        strcmp(tokens[1], "SENSORS") == 0) {
        handle_list_sensors(client_fd, info, raw_msg);
        return;
    }

    /* ---- GET DATA ---- */
    if (strcmp(tokens[0], "GET") == 0 && ntokens >= 2 &&
        strcmp(tokens[1], "DATA") == 0) {
        handle_get_data(client_fd, tokens, ntokens, info, raw_msg);
        return;
    }

    /* ---- Comando no reconocido ---- */
    char resp[] = "ERROR 400 BAD_REQUEST\n";
    send_response(client_fd, resp);
    log_event(info->ip, info->port, raw_msg, resp);
}

/* ========================== LIMPIEZA AL DESCONECTAR =======================*/

/*
 * cleanup_client() — se llama cuando un cliente se desconecta (de forma
 * limpia o abrupta).  Marca sensores como INACTIVE y elimina operadores.
 */
static void cleanup_client(int fd, client_info_t *info, int abrupt)
{
    char ts[64];
    get_timestamp_str(ts, sizeof(ts));

    int sensor_mutex_released = 0;

    /* 1. Tomar sensor_mutex y buscar si el fd corresponde a un sensor */
    pthread_mutex_lock(&sensor_mutex);
    for (int i = 0; i < sensor_count; i++) {
        if (sensors[i].client_fd == fd &&
            strcmp(sensors[i].status, "ACTIVE") == 0) {
            strcpy(sensors[i].status, "INACTIVE");
            sensors[i].client_fd = -1;

            /* Si la desconexión fue abrupta, notificar operadores */
            if (abrupt) {
                char alert[128];
                snprintf(alert, sizeof(alert), "SENSOR_DOWN %s\n", sensors[i].id);
                char sensor_id_copy[32];
                strncpy(sensor_id_copy, sensors[i].id, sizeof(sensor_id_copy));
                sensor_id_copy[sizeof(sensor_id_copy) - 1] = '\0';

                /* Liberar sensor_mutex ANTES de broadcast (evita deadlock) */
                pthread_mutex_unlock(&sensor_mutex);
                sensor_mutex_released = 1;

                broadcast_to_operators(alert);

                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg),
                         "Sensor %s disconnected abruptly", sensor_id_copy);
                log_event(info->ip, info->port, log_msg, alert);
            }
            break;
        }
    }
    if (!sensor_mutex_released) {
        pthread_mutex_unlock(&sensor_mutex);
    }

    /* 2. Buscar si este fd corresponde a un operador y eliminarlo */
    pthread_mutex_lock(&operator_mutex);
    for (int i = 0; i < operator_count; i++) {
        if (operators[i].client_fd == fd) {
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg),
                     "Operator %s disconnected", operators[i].username);

            operators[i] = operators[operator_count - 1];
            operator_count--;

            pthread_mutex_unlock(&operator_mutex);
            log_event(info->ip, info->port, log_msg, "(disconnect)");
            return;
        }
    }
    pthread_mutex_unlock(&operator_mutex);
}

/* ========================== HILO POR CLIENTE ==============================*/

/*
 * handle_client() — función ejecutada por cada hilo.
 * Lee mensajes del socket delimitados por '\n' y los despacha.
 */
void *handle_client(void *arg)
{
    client_info_t *info = (client_info_t *)arg;
    int            fd   = info->fd;

    char ts[64];
    get_timestamp_str(ts, sizeof(ts));

    pthread_mutex_lock(&log_mutex);
    printf("[%s] New connection from %s:%d (fd=%d)\n",
           ts, info->ip, info->port, fd);
    if (log_file) {
        fprintf(log_file, "[%s] New connection from %s:%d (fd=%d)\n",
                ts, info->ip, info->port, fd);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);

    /*
     * Buffer acumulador para manejar mensajes parciales.
     * Acumulamos datos hasta encontrar '\n'.
     */
    char accumulator[BUFFER_SIZE * 2];
    int  acc_len = 0;

    while (1) {
        char recv_buf[BUFFER_SIZE];
        ssize_t n = recv(fd, recv_buf, sizeof(recv_buf) - 1, 0);

        if (n <= 0) {
            /* n == 0 → desconexión limpia del socket
             * n <  0 → error de red */
            if (n == 0) {
                /* Desconexión abrupta a nivel TCP */
                cleanup_client(fd, info, 1);
            } else {
                cleanup_client(fd, info, 1);
            }
            break;
        }

        recv_buf[n] = '\0';

        /* Acumular datos recibidos */
        if (acc_len + n >= (int)sizeof(accumulator) - 1) {
            /* Overflow del buffer → descartar y reportar error */
            acc_len = 0;
            char resp[] = "ERROR 500 BUFFER_OVERFLOW\n";
            send_response(fd, resp);
            continue;
        }
        memcpy(accumulator + acc_len, recv_buf, n);
        acc_len += n;
        accumulator[acc_len] = '\0';

        /* Procesar todos los mensajes completos (terminados en '\n') */
        char *line_start = accumulator;
        char *newline;

        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';  /* terminar la línea */

            /* Ignorar líneas vacías */
            if (strlen(line_start) > 0) {
                char message[BUFFER_SIZE];
                strncpy(message, line_start, sizeof(message) - 1);
                message[sizeof(message) - 1] = '\0';
                parse_and_dispatch(fd, message, info);
            }

            line_start = newline + 1;
        }

        /* Mover datos restantes (mensaje parcial) al inicio del acumulador */
        int remaining = acc_len - (int)(line_start - accumulator);
        if (remaining > 0) {
            memmove(accumulator, line_start, remaining);
        }
        acc_len = remaining;
        accumulator[acc_len] = '\0';
    }

    close(fd);

    get_timestamp_str(ts, sizeof(ts));
    pthread_mutex_lock(&log_mutex);
    printf("[%s] Connection closed: %s:%d (fd=%d)\n",
           ts, info->ip, info->port, fd);
    if (log_file) {
        fprintf(log_file, "[%s] Connection closed: %s:%d (fd=%d)\n",
                ts, info->ip, info->port, fd);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);

    free(info);
    return NULL;
}

/* ========================== SERVIDOR HTTP =================================*/

/* Estructura para pasar el puerto HTTP al hilo */
typedef struct {
    char port[16];
} http_thread_arg_t;

/*
 * http_send() — envía una respuesta HTTP completa (status + headers + body).
 */
static void http_send_response(int fd, int status_code, const char *status_text,
                               const char *content_type, const char *body)
{
    char header[BUFFER_SIZE];
    int body_len = body ? (int)strlen(body) : 0;
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "\r\n",
             status_code, status_text, content_type, body_len);
    send(fd, header, strlen(header), MSG_NOSIGNAL);
    if (body && body_len > 0) {
        send(fd, body, body_len, MSG_NOSIGNAL);
    }
}

/* Página de login HTML */
static const char *LOGIN_HTML =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<title>IoT Monitoring - Login</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;"
    "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
    ".card{background:#16213e;padding:40px;border-radius:12px;box-shadow:0 8px 32px rgba(0,0,0,.3);"
    "width:340px}"
    "h2{text-align:center;color:#0f9;margin-bottom:24px}"
    "label{display:block;margin:10px 0 4px;font-size:14px}"
    "input,select{width:100%;padding:10px;border:1px solid #333;border-radius:6px;"
    "background:#0f3460;color:#eee;box-sizing:border-box;font-size:14px}"
    "button{width:100%;padding:12px;margin-top:20px;background:#0f9;color:#1a1a2e;"
    "border:none;border-radius:6px;font-size:16px;font-weight:bold;cursor:pointer}"
    "button:hover{background:#0da}"
    "</style></head><body>"
    "<div class='card'><h2>IoT Monitoring</h2>"
    "<form method='POST' action='/login'>"
    "<label>Usuario</label><input name='usuario' required>"
    "<label>Password</label><input name='password' type='password' required>"
    "<label>Rol</label><select name='rol'>"
    "<option value='OPERATOR'>OPERATOR</option>"
    "<option value='SENSOR'>SENSOR</option></select>"
    "<button type='submit'>Ingresar</button></form></div></body></html>";

/*
 * http_parse_form_value() — extrae un valor de un body URL-encoded.
 * Busca "key=valor" y copia el valor a dst.
 */
static int http_parse_form_value(const char *body, const char *key,
                                 char *dst, size_t dst_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return -1;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '&' && i < dst_len - 1) {
        if (*p == '+') { dst[i++] = ' '; p++; }
        else { dst[i++] = *p++; }
    }
    dst[i] = '\0';
    return 0;
}

/*
 * http_handle_request() — procesa una petición HTTP individual.
 */
static void http_handle_request(int client_fd, const char *ip, int port)
{
    char buf[BUFFER_SIZE * 2];
    int total = 0;

    /* Leer la petición completa (headers + body) */
    while (total < (int)sizeof(buf) - 1) {
        ssize_t n = recv(client_fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        /* Si encontramos el fin de headers, verificar Content-Length */
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            /* Buscar Content-Length */
            char *cl = strstr(buf, "Content-Length:");
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) {
                int clen = atoi(cl + 15);
                int body_start = (int)(hdr_end + 4 - buf);
                int body_received = total - body_start;
                if (body_received >= clen) break;
            } else {
                break; /* No hay body */
            }
        }
    }

    if (total <= 0) { close(client_fd); return; }

    /* Parsear método y path */
    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    char log_recv[512];
    snprintf(log_recv, sizeof(log_recv), "HTTP %s %s", method, path);

    /* ---- GET / ---- */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send_response(client_fd, 200, "OK", "text/html", LOGIN_HTML);
        log_event((char *)ip, port, log_recv, "200 OK (login page)");
    }
    /* ---- GET /status ---- */
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) {
        int active = 0, total_s = 0, ops = 0;
        pthread_mutex_lock(&sensor_mutex);
        total_s = sensor_count;
        for (int i = 0; i < sensor_count; i++) {
            if (strcmp(sensors[i].status, "ACTIVE") == 0) active++;
        }
        pthread_mutex_unlock(&sensor_mutex);
        pthread_mutex_lock(&operator_mutex);
        ops = operator_count;
        pthread_mutex_unlock(&operator_mutex);

        char json[256];
        snprintf(json, sizeof(json),
                 "{\"sensors_active\":%d,\"sensors_total\":%d,"
                 "\"operators_connected\":%d}", active, total_s, ops);
        http_send_response(client_fd, 200, "OK", "application/json", json);
        log_event((char *)ip, port, log_recv, json);
    }
    /* ---- POST /login ---- */
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/login") == 0) {
        char *body = strstr(buf, "\r\n\r\n");
        if (!body) {
            http_send_response(client_fd, 400, "Bad Request",
                               "application/json", "{\"error\":\"no body\"}");
            log_event((char *)ip, port, log_recv, "400 no body");
            close(client_fd);
            return;
        }
        body += 4;

        char usuario[32] = {0}, rol[16] = {0};
        if (http_parse_form_value(body, "usuario", usuario, sizeof(usuario)) < 0 ||
            http_parse_form_value(body, "rol", rol, sizeof(rol)) < 0 ||
            strlen(usuario) == 0 || strlen(rol) == 0) {
            http_send_response(client_fd, 400, "Bad Request",
                               "application/json",
                               "{\"error\":\"missing usuario or rol\"}");
            log_event((char *)ip, port, log_recv, "400 missing fields");
            close(client_fd);
            return;
        }

        if (strcmp(rol, "OPERATOR") == 0) {
            /* Registrar operador (sin socket TCP asociado, fd = -1) */
            pthread_mutex_lock(&operator_mutex);
            for (int i = 0; i < operator_count; i++) {
                if (strcmp(operators[i].username, usuario) == 0) {
                    pthread_mutex_unlock(&operator_mutex);
                    http_send_response(client_fd, 409, "Conflict",
                                       "application/json",
                                       "{\"error\":\"user already exists\"}");
                    log_event((char *)ip, port, log_recv, "409 conflict");
                    close(client_fd);
                    return;
                }
            }
            if (operator_count >= MAX_OPERATORS) {
                pthread_mutex_unlock(&operator_mutex);
                http_send_response(client_fd, 500, "Internal Server Error",
                                   "application/json",
                                   "{\"error\":\"max operators reached\"}");
                close(client_fd);
                return;
            }
            operator_t *op = &operators[operator_count];
            strncpy(op->username, usuario, sizeof(op->username) - 1);
            op->username[sizeof(op->username) - 1] = '\0';
            op->client_fd = -1; /* HTTP, no hay socket persistente */
            strncpy(op->ip, ip, sizeof(op->ip) - 1);
            op->ip[sizeof(op->ip) - 1] = '\0';
            op->port = port;
            operator_count++;
            pthread_mutex_unlock(&operator_mutex);

            char resp_json[128];
            snprintf(resp_json, sizeof(resp_json),
                     "{\"status\":\"ok\",\"user\":\"%s\"}", usuario);
            http_send_response(client_fd, 200, "OK",
                               "application/json", resp_json);
            log_event((char *)ip, port, log_recv, resp_json);
        } else {
            /* Rol SENSOR u otro: solo confirmar (no se puede crear sensor por HTTP) */
            char resp_json[128];
            snprintf(resp_json, sizeof(resp_json),
                     "{\"status\":\"ok\",\"user\":\"%s\",\"note\":\"sensor must connect via TCP\"}",
                     usuario);
            http_send_response(client_fd, 200, "OK",
                               "application/json", resp_json);
            log_event((char *)ip, port, log_recv, resp_json);
        }
    }
    /* ---- 404 ---- */
    else {
        http_send_response(client_fd, 404, "Not Found",
                           "application/json", "{\"error\":\"not found\"}");
        log_event((char *)ip, port, log_recv, "404 Not Found");
    }

    close(client_fd);
}

/*
 * http_server_thread() — hilo principal del servidor HTTP.
 * Crea un socket TCP, hace bind/listen y acepta conexiones HTTP.
 */
static void *http_server_thread(void *arg)
{
    http_thread_arg_t *harg = (http_thread_arg_t *)arg;
    const char *port = harg->port;

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        fprintf(stderr, "HTTP: getaddrinfo error for port %s\n", port);
        free(harg);
        return NULL;
    }

    int http_fd = -1;
    for (p = res; p; p = p->ai_next) {
        http_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (http_fd < 0) continue;
        int opt = 1;
        setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(http_fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(http_fd);
        http_fd = -1;
    }
    freeaddrinfo(res);

    if (http_fd < 0) {
        fprintf(stderr, "HTTP: could not bind on port %s\n", port);
        free(harg);
        return NULL;
    }

    if (listen(http_fd, SOMAXCONN) < 0) {
        perror("HTTP listen");
        close(http_fd);
        free(harg);
        return NULL;
    }

    printf("HTTP Server started on port %s\n", port);
    fflush(stdout);

    while (1) {
        struct sockaddr_storage ca;
        socklen_t ca_len = sizeof(ca);
        int cfd = accept(http_fd, (struct sockaddr *)&ca, &ca_len);
        if (cfd < 0) { continue; }

        char ip[INET6_ADDRSTRLEN];
        int cport = 0;
        if (ca.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&ca;
            inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
            cport = ntohs(s->sin_port);
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&ca;
            inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
            cport = ntohs(s->sin6_port);
        }

        http_handle_request(cfd, ip, cport);
    }

    close(http_fd);
    free(harg);
    return NULL;
}

/* ========================== MAIN ==========================================*/

int main(int argc, char *argv[])
{
    /* ------- Validar argumentos ------- */
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <puerto_tcp> <puerto_http> <archivoDeLogs>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *port_str      = argv[1];
    const char *http_port_str = argv[2];
    const char *log_filename  = argv[3];

    /* ------- Ignorar SIGPIPE ------- */
    /*
     * Cuando un cliente se desconecta y el servidor intenta escribir
     * en el socket, se genera SIGPIPE.  Lo ignoramos para que send()
     * simplemente retorne -1 con errno = EPIPE.
     */
    signal(SIGPIPE, SIG_IGN);

    /* ------- Abrir archivo de log ------- */
    log_file = fopen(log_filename, "a");
    if (!log_file) {
        fprintf(stderr, "Error: no se pudo abrir el archivo de log '%s': %s\n",
                log_filename, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* ------- Resolver la dirección local con getaddrinfo() ------- */
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;    /* IPv4 o IPv6 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP          */
    hints.ai_flags    = AI_PASSIVE;   /* Para bind()  */

    int gai_status = getaddrinfo(NULL, port_str, &hints, &res);
    if (gai_status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(gai_status));
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    /* ------- Crear el socket maestro ------- */
    int server_fd = -1;

    for (p = res; p != NULL; p = p->ai_next) {
        server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_fd < 0) continue;

        /* Permitir reutilizar el puerto inmediatamente al reiniciar */
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(server_fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;  /* Éxito */
        }

        close(server_fd);
        server_fd = -1;
    }

    freeaddrinfo(res);

    if (server_fd < 0) {
        fprintf(stderr, "Error: no se pudo hacer bind en el puerto %s\n", port_str);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    /* ------- Escuchar conexiones ------- */
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    printf("IoT Monitoring Server started on port %s\n", port_str);
    fflush(stdout);

    /* Log del inicio del servidor */
    {
        char ts[64];
        get_timestamp_str(ts, sizeof(ts));
        fprintf(log_file, "[%s] Server started on port %s (HTTP on %s)\n",
                ts, port_str, http_port_str);
        fflush(log_file);
    }

    /* ------- Lanzar el hilo del servidor HTTP ------- */
    {
        http_thread_arg_t *harg = malloc(sizeof(http_thread_arg_t));
        if (harg) {
            strncpy(harg->port, http_port_str, sizeof(harg->port) - 1);
            harg->port[sizeof(harg->port) - 1] = '\0';
            pthread_t http_tid;
            if (pthread_create(&http_tid, NULL, http_server_thread, harg) != 0) {
                perror("pthread_create (HTTP)");
                free(harg);
            } else {
                pthread_detach(http_tid);
            }
        }
    }

    /* ------- Loop principal: aceptar conexiones ------- */
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue;  /* No abortar el servidor por un fallo en accept */
        }

        /* Extraer IP y puerto del cliente */
        client_info_t *info = malloc(sizeof(client_info_t));
        if (!info) {
            fprintf(stderr, "Error: malloc failed for client_info\n");
            close(client_fd);
            continue;
        }
        info->fd = client_fd;

        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &s->sin_addr, info->ip, sizeof(info->ip));
            info->port = ntohs(s->sin_port);
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, info->ip, sizeof(info->ip));
            info->port = ntohs(s->sin6_port);
        }

        /* Crear un hilo para manejar al cliente */
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void *)info) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(info);
            continue;
        }

        /* Desligar el hilo para que se limpie automáticamente al terminar */
        pthread_detach(tid);
    }

    /* Limpieza (teóricamente nunca se alcanza) */
    close(server_fd);
    fclose(log_file);
    return 0;
}
