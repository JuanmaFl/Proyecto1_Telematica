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


/* Aqui estamos definiendo los limites del sistemas que serian el limite de sensiores
operadores, las mediciones en el historial y el tamaño del buffer*/
#define MAX_SENSORS    100
#define MAX_OPERATORS   50
#define HISTORY_SIZE    10
#define BUFFER_SIZE   4096
#define MAX_TOKENS      16

/*Aqui estamos definiendo los limites de alerta para cada tipo de sensor*/
#define THRESHOLD_TEMPERATURE  80.0f
#define THRESHOLD_VIBRATION    10.0f
#define THRESHOLD_ENERGY      500.0f


/* Información de un sensor registrado, con los tipos de sensiores que hay, su estado de activo
o inactivo, el ultimo valor que se recibio, la fecha y hora en la que se recibio, el historial de mediciones
y el file descriptor del socket del sensor */
typedef struct {
    char  id[32];
    char  type[32];          
    char  status[16];        
    float last_value;
    long  last_timestamp;
    float history[HISTORY_SIZE];
    int   history_count;     
    int   client_fd;         
} sensor_t;

/* Información de un operador conectado guardando su socket para poder enviarle alertas en tiempo
real */
typedef struct {
    char username[32];
    int  client_fd;
    char ip[INET6_ADDRSTRLEN];
    int  port;
} operator_t;

/* Información que se pasa al hilo de cada cliente nuevo que se conecta */
typedef struct {
    int  fd;
    char ip[INET6_ADDRSTRLEN];
    int  port;
} client_info_t;



/* Listas compartidas entre todos los hilos */
static sensor_t   sensors[MAX_SENSORS];
static int        sensor_count = 0;

static operator_t operators[MAX_OPERATORS];
static int        operator_count = 0;

/* Mutexes para proteger las estructuras compartidas, protegen la lista de sensores, la
lista de operadores y el archivo de log respectivamente */
static pthread_mutex_t sensor_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t operator_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex      = PTHREAD_MUTEX_INITIALIZER;

/* Archivo de log */
static FILE *log_file = NULL;


/*Funciones auxiliares: get_timestamp_str() formatea la fecha y hora actual
   para los logs, y send_response() envía un mensaje al cliente asegurándose
   de que termine en '\n' como exige el protocolo */
static void get_timestamp_str(char *buf, size_t len)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
}

static int send_response(int fd, const char *msg)
{
    size_t len = strlen(msg);
    
    if (len == 0) return 0;

    if (send(fd, msg, len, MSG_NOSIGNAL) < 0)
        return -1;

    if (msg[len - 1] != '\n') {
        if (send(fd, "\n", 1, MSG_NOSIGNAL) < 0)
            return -1;
    }
    return 0;
}


/* Aqui seegistra cada evento en consola y en el archivo de log con el formato:
   [TIMESTAMP] [IP:PUERTO] [RECIBIDO: msg] [ENVIADO: respuesta]
   Usa log_mutex para evitar que los mensajes de distintos hilos
   se mezclen en la salida*/
void log_event(char *ip, int port, char *received, char *sent)
{
    char ts[64];
    get_timestamp_str(ts, sizeof(ts));

    /* Copiar las cadenas para poder limpiar saltos de linea */
    char recv_clean[BUFFER_SIZE];
    char sent_clean[BUFFER_SIZE];
    strncpy(recv_clean, received ? received : "(none)", sizeof(recv_clean) - 1);
    recv_clean[sizeof(recv_clean) - 1] = '\0';
    strncpy(sent_clean, sent ? sent : "(none)", sizeof(sent_clean) - 1);
    sent_clean[sizeof(sent_clean) - 1] = '\0';

    /* Eliminar "\n" finales para que el log sea de una sola linea */
    size_t rlen = strlen(recv_clean);
    if (rlen > 0 && recv_clean[rlen - 1] == '\n') recv_clean[rlen - 1] = '\0';
    size_t slen = strlen(sent_clean);
    if (slen > 0 && sent_clean[slen - 1] == '\n') sent_clean[slen - 1] = '\0';

    pthread_mutex_lock(&log_mutex);

    printf("[%s] [%s:%d] [RECIBIDO: %s] [ENVIADO: %s]\n",
           ts, ip, port, recv_clean, sent_clean);
    fflush(stdout);

    if (log_file) {
        fprintf(log_file, "[%s] [%s:%d] [RECIBIDO: %s] [ENVIADO: %s]\n",
                ts, ip, port, recv_clean, sent_clean);
        fflush(log_file);
    }

    pthread_mutex_unlock(&log_mutex);
}


/* Envia mensajes a todos los operadores que estan conectados y se llama
cuando supera el limite que ya esta definido del sensor o se desconecta*/
void broadcast_to_operators(char *message)
{
    pthread_mutex_lock(&operator_mutex);
    for (int i = 0; i < operator_count; i++) {
        send_response(operators[i].client_fd, message);
    }
    pthread_mutex_unlock(&operator_mutex);
}


/*Aqui compara el valor recibido con el limite definido por cada sensor del programa y si alguno supera
manda el mensaje de alerta correspondiente a todos los operadores*/
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


/*BÚSQUEDA DE SENSOR*/
static int find_sensor(const char *id)
{
    for (int i = 0; i < sensor_count; i++) {
        if (strcmp(sensors[i].id, id) == 0)
            return i;
    }
    return -1;
}


/*TOKENIZER*/
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


/*HANDLERS DE PROTOCOLO*/
/* Valida el tipo de sensor, si existe y esta inactivo, lo activa y si ya existe 
y esta activo, envia el error correspondiente*/
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

    if (strcmp(type, "TEMPERATURE") != 0 &&
        strcmp(type, "VIBRATION")   != 0 &&
        strcmp(type, "ENERGY")      != 0) {
        snprintf(response, sizeof(response), "ERROR 400 INVALID_SENSOR_TYPE\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    pthread_mutex_lock(&sensor_mutex);

    int idx = find_sensor(id);
    if (idx >= 0) {
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

    if (sensor_count >= MAX_SENSORS) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 500 MAX_SENSORS_REACHED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

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

/* Recibe los datos del sensor, actualiza su estado y envia una respuesta*/
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

    if (strcmp(s->status, "ACTIVE") != 0 || s->client_fd != fd) {
        pthread_mutex_unlock(&sensor_mutex);
        snprintf(response, sizeof(response), "ERROR 401 UNAUTHORIZED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

    s->last_value    = value;
    s->last_timestamp = timestamp;

    int hist_idx = s->history_count % HISTORY_SIZE;
    s->history[hist_idx] = value;
    s->history_count++;

    sensor_t sensor_copy = *s;

    pthread_mutex_unlock(&sensor_mutex);

    snprintf(response, sizeof(response), "OK DATA RECEIVED\n");
    send_response(fd, response);
    log_event(info->ip, info->port, raw_msg, response);

    check_alert(&sensor_copy, value);
}

/*Verifica que el sensor este asociado al cliente antes de desconectarlo*/
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

/*Registra un nuevo operador y guarda su información, pero antes verifica que no exista*/
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

    if (operator_count >= MAX_OPERATORS) {
        pthread_mutex_unlock(&operator_mutex);
        snprintf(response, sizeof(response), "ERROR 500 MAX_OPERATORS_REACHED\n");
        send_response(fd, response);
        log_event(info->ip, info->port, raw_msg, response);
        return;
    }

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

/*Lista todos los sensores registrados en su formato especifico*/
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

/*Aqui verifica todo el historial de datos del sensor, que serian hasta 10 que anterormente esta
definido que sea asi y los pone en una lista del mas antiguo al mas reciente*/
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


    int start;
    if (total <= HISTORY_SIZE) {
        start = 0;
    } else {
        start = total % HISTORY_SIZE;  
    }

    char all_responses[BUFFER_SIZE];
    int  off = 0;

    for (int i = 0; i < count; i++) {
        int hi = (start + i) % HISTORY_SIZE;
        off += snprintf(all_responses + off, sizeof(all_responses) - off,
                        "DATA_RESPONSE %s %.2f %ld\n",
                        id, s->history[hi], s->last_timestamp);
    }

    pthread_mutex_unlock(&sensor_mutex);

    send(fd, all_responses, strlen(all_responses), MSG_NOSIGNAL);
    log_event(info->ip, info->port, raw_msg, all_responses);
}

/* El parser y dispatcher se encarga de analizar el mensaje recibido, lo tokeniza 
y lo despacha al handler correspondiente*/
void parse_and_dispatch(int client_fd, char *message, client_info_t *info)
{
    char raw_msg[BUFFER_SIZE];
    strncpy(raw_msg, message, sizeof(raw_msg) - 1);
    raw_msg[sizeof(raw_msg) - 1] = '\0';

    char *tokens[MAX_TOKENS];
    int   ntokens = tokenize(message, tokens, MAX_TOKENS);

    if (ntokens == 0) {
        char resp[] = "ERROR 400 BAD_REQUEST\n";
        send_response(client_fd, resp);
        log_event(info->ip, info->port, raw_msg, resp);
        return;
    }

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

    if (strcmp(tokens[0], "DATA") == 0) {
        handle_sensor_data(client_fd, tokens, ntokens, info, raw_msg);
        return;
    }

    if (strcmp(tokens[0], "DISCONNECT") == 0) {
        handle_disconnect(client_fd, tokens, ntokens, info, raw_msg);
        return;
    }

    if (strcmp(tokens[0], "LIST") == 0 && ntokens >= 2 &&
        strcmp(tokens[1], "SENSORS") == 0) {
        handle_list_sensors(client_fd, info, raw_msg);
        return;
    }

    if (strcmp(tokens[0], "GET") == 0 && ntokens >= 2 &&
        strcmp(tokens[1], "DATA") == 0) {
        handle_get_data(client_fd, tokens, ntokens, info, raw_msg);
        return;
    }

    char resp[] = "ERROR 400 BAD_REQUEST\n";
    send_response(client_fd, resp);
    log_event(info->ip, info->port, raw_msg, resp);
}


/*Se ejecuta cuando un cliente cierra la conexión, ya sea de forma normal o por una caída inesperada.
 */
static void cleanup_client(int fd, client_info_t *info, int abrupt)
{
    char ts[64];
    get_timestamp_str(ts, sizeof(ts));

    int sensor_mutex_released = 0;


    pthread_mutex_lock(&sensor_mutex);
    for (int i = 0; i < sensor_count; i++) {
        if (sensors[i].client_fd == fd &&
            strcmp(sensors[i].status, "ACTIVE") == 0) {
            strcpy(sensors[i].status, "INACTIVE");
            sensors[i].client_fd = -1;

            if (abrupt) {
                char alert[128];
                snprintf(alert, sizeof(alert), "SENSOR_DOWN %s\n", sensors[i].id);
                char sensor_id_copy[32];
                strncpy(sensor_id_copy, sensors[i].id, sizeof(sensor_id_copy));
                sensor_id_copy[sizeof(sensor_id_copy) - 1] = '\0';

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


/*Esta función es ejecutada por un hilo independiente por cada cliente
que se conecta al servidor. Su responsabilidad es manejar toda la
comunicación con ese cliente mientras la conexión esté activa.
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

    char accumulator[BUFFER_SIZE * 2];
    int  acc_len = 0;

    while (1) {
        char recv_buf[BUFFER_SIZE];
        ssize_t n = recv(fd, recv_buf, sizeof(recv_buf) - 1, 0);

        if (n <= 0) {
            if (n == 0) {
                cleanup_client(fd, info, 1);
            } else {
                cleanup_client(fd, info, 1);
            }
            break;
        }

        recv_buf[n] = '\0';

        if (acc_len + n >= (int)sizeof(accumulator) - 1) {
            acc_len = 0;
            char resp[] = "ERROR 500 BUFFER_OVERFLOW\n";
            send_response(fd, resp);
            continue;
        }
        memcpy(accumulator + acc_len, recv_buf, n);
        acc_len += n;
        accumulator[acc_len] = '\0';

        char *line_start = accumulator;
        char *newline;

        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';  

            if (strlen(line_start) > 0) {
                char message[BUFFER_SIZE];
                strncpy(message, line_start, sizeof(message) - 1);
                message[sizeof(message) - 1] = '\0';
                parse_and_dispatch(fd, message, info);
            }

            line_start = newline + 1;
        }

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

/*SERVIDOR HTTP*/
/* Estructura para pasar el puerto HTTP al hilo */
typedef struct {
    char port[16];
} http_thread_arg_t;

/*Envía una respuesta HTTP completa*/
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

/*Extrae un valor de un body URL-encoded*/
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

/*Procesa una petición HTTP individual*/
static void http_handle_request(int client_fd, const char *ip, int port)
{
    char buf[BUFFER_SIZE * 2];
    int total = 0;

    while (total < (int)sizeof(buf) - 1) {
        ssize_t n = recv(client_fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            char *cl = strstr(buf, "Content-Length:");
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) {
                int clen = atoi(cl + 15);
                int body_start = (int)(hdr_end + 4 - buf);
                int body_received = total - body_start;
                if (body_received >= clen) break;
            } else {
                break; 
            }
        }
    }

    if (total <= 0) { close(client_fd); return; }

    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    char log_recv[512];
    snprintf(log_recv, sizeof(log_recv), "HTTP %s %s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send_response(client_fd, 200, "OK", "text/html", LOGIN_HTML);
        log_event((char *)ip, port, log_recv, "200 OK (login page)");
    }
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
            op->client_fd = -1;
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
            char resp_json[128];
            snprintf(resp_json, sizeof(resp_json),
                     "{\"status\":\"ok\",\"user\":\"%s\",\"note\":\"sensor must connect via TCP\"}",
                     usuario);
            http_send_response(client_fd, 200, "OK",
                               "application/json", resp_json);
            log_event((char *)ip, port, log_recv, resp_json);
        }
    }
    else {
        http_send_response(client_fd, 404, "Not Found",
                           "application/json", "{\"error\":\"not found\"}");
        log_event((char *)ip, port, log_recv, "404 Not Found");
    }

    close(client_fd);
}

/* Implementa el servidor HTTP como un hilo independiente*/
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

/*El main se encarga de inicializar el servidor y manejar las conexiones entrantes, 
creando el socket tcp y lanzando el hilo que maneja las conexiones*/

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <puerto_tcp> <puerto_http> <archivoDeLogs>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *port_str      = argv[1];
    const char *http_port_str = argv[2];
    const char *log_filename  = argv[3];


    signal(SIGPIPE, SIG_IGN);


    log_file = fopen(log_filename, "a");
    if (!log_file) {
        fprintf(stderr, "Error: no se pudo abrir el archivo de log '%s': %s\n",
                log_filename, strerror(errno));
        exit(EXIT_FAILURE);
    }

   
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;    
    hints.ai_socktype = SOCK_STREAM;  
    hints.ai_flags    = AI_PASSIVE;   

    int gai_status = getaddrinfo(NULL, port_str, &hints, &res);
    if (gai_status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(gai_status));
        fclose(log_file);
        exit(EXIT_FAILURE);
    }


    int server_fd = -1;

    for (p = res; p != NULL; p = p->ai_next) {
        server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_fd < 0) continue;

    
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(server_fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;  
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

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    printf("IoT Monitoring Server started on port %s\n", port_str);
    fflush(stdout);

    {
        char ts[64];
        get_timestamp_str(ts, sizeof(ts));
        fprintf(log_file, "[%s] Server started on port %s (HTTP on %s)\n",
                ts, port_str, http_port_str);
        fflush(log_file);
    }

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

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue;  
        }

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

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void *)info) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(info);
            continue;
        }

        pthread_detach(tid);
    }

    close(server_fd);
    fclose(log_file);
    return 0;
}
