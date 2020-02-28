#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <regex.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <arpa/inet.h>

#include <mosquitto.h>

#define CONN_ERR_TIMEOUT 1
#define GET_MESSAGE "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n"
#define MATCH_DATA "\\{\"timestamp\":\"(.*)\",\"temp\":(.*),\"pressure\":(.*),\"humidity\":(.*)\\}"
#define MATCH_CODE "^HTTP/1.1 ([0-9]{3,3}) (.*)$"
#define GROUPS_DATA_LEN 5
#define GROUPS_CODE_LEN 3
#define RECV_LEN 1024
#define ROOT "/"
#define UNSTABLE "/unstable"
#define CODE_OK 200
#define END '\0'
#define TIMESTAMP_LEN 34
#define DATA_LEN 16
#define ID_LEN 256
#define PAYLOAD "{timestamp\":\"%s\",\"temp\":%s}"
#define PAYLOAD_LEN 512
#define TOPIC "test"
#define TOPIC_LOG "log"

#define NRM  "\x1B[0m"
#define RED  "\x1B[31m"
#define GRN  "\x1B[32m"
#define YEL  "\x1B[33m"
#define BLU  "\x1B[34m"
#define WHT  "\x1B[37m"

static int debug = 1;
static int exit_flag = 0;

typedef struct sensor_opt
{
    const char *host;
    int port;
    const char *uri;
    unsigned timeout;
    const char *id;
    const char *mqtt_host;
    int mqtt_port;
    int mqtt_keepalive;
    const char *mqtt_bind_address;
    const char *topic;
    const char *log;
} sensor_opt;

typedef struct params
{
    char timestamp[TIMESTAMP_LEN];
    char temp[DATA_LEN];
    char pressure[DATA_LEN];
    char humidity[DATA_LEN];
} params;

struct sockaddr_in *get_sin(const char *host, int port)
{
    struct sockaddr_in *s = NULL;
    s = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
    if (!s)
    {
        fprintf(stderr, RED"Could not alloc memory\n"NRM);
        return NULL;
    }

    s->sin_addr.s_addr = inet_addr(host);
    s->sin_family = AF_INET;
    s->sin_port = htons(port);

    return s;
}

void log_callback(struct mosquitto *mosq, void *obj, int level, const char *str)
{
    (void)mosq;
    sensor_opt *opt = (sensor_opt *)obj;

    printf(GRN"%s: %d: %s\n"NRM, opt->id, level, str);
}

void cb_disconnect(struct mosquitto *mosq, void *obj, int rc, const mosquitto_property *properties)
{
    (void)mosq;
    (void)properties;
    sensor_opt *opt = (sensor_opt *)obj;

    printf(YEL"Disconnect %s %d\n"NRM, opt->id, rc);
}

void cb_connect(struct mosquitto *mosq, void *obj, int result, int flags,
                const mosquitto_property *properties)
{
    (void)flags;
    (void)properties;
    sensor_opt *opt = (sensor_opt *)obj;

    if (result)
    {
        fprintf(stderr, RED"Could not connect to mqtt broker\n"NRM);
        mosquitto_disconnect_v5(mosq, 0, NULL);
    }
    else
    {
        if (debug)
        {
            fprintf(stderr, YEL"Connected to: %s:%d\n"NRM, opt->mqtt_host, opt->mqtt_port);
        }
    }
}

void cb_publish(struct mosquitto *mosq, void *obj, int mid, int reason_code,
                const mosquitto_property *properties)
{
    (void)mosq;
    (void)properties;
    sensor_opt *opt = (sensor_opt *)obj;

    if (reason_code > 127)
    {
        fprintf(stderr, RED"Could not publish: %d %s\n"NRM, mid, mosquitto_reason_string(reason_code));
    }
    else
    {
        if (debug)
        {
            fprintf(stderr, YEL"Published to: %s:%d\n"NRM, opt->mqtt_host, opt->mqtt_port);
        }
    }
}

void mqtt_publish_error(int rc)
{
    fprintf(stderr, RED"Cannot publish:\n"NRM);
    switch (rc)
    {
        case MOSQ_ERR_INVAL:
            fprintf(stderr, RED"Error: Invalid input. Does your topic contain '+' or '#'?\n"NRM);
            break;
        case MOSQ_ERR_NOMEM:
            fprintf(stderr, RED"Error: Out of memory when trying to publish message.\n"NRM);
            break;
        case MOSQ_ERR_NO_CONN:
            fprintf(stderr, RED"Error: Client not connected when trying to publish.\n"NRM);
            break;
        case MOSQ_ERR_PROTOCOL:
            fprintf(stderr, RED"Error: Protocol error when communicating with broker.\n"NRM);
            break;
        case MOSQ_ERR_PAYLOAD_SIZE:
            fprintf(stderr, RED"Error: Message payload is too large.\n"NRM);
            break;
        case MOSQ_ERR_QOS_NOT_SUPPORTED:
            fprintf(stderr, RED"Error: Message QoS not supported on broker, try a lower QoS.\n"NRM);
            break;
    }
}

void *sensor(void *data)
{
    int fd;
    sensor_opt *opt = (sensor_opt *)data;
    struct sockaddr_in *addr = NULL;
    struct timespec tw, tr, ts;
    char *req = NULL;
    char *res = NULL;
    size_t req_len = sizeof(GET_MESSAGE) - 4 + strlen(opt->uri) + strlen(opt->host);
    size_t res_len = RECV_LEN;
    regex_t re_data, re_code;
    size_t data_len;
    size_t groups_data = GROUPS_DATA_LEN;
    size_t groups_code = GROUPS_CODE_LEN;
    regmatch_t groups_data_array[groups_data];
    regmatch_t groups_code_array[groups_code];
    char code[4];
    unsigned ucode;
    params *pr =  NULL;
    sigset_t sigset_block, sigset_restore, sigset_pending;
    struct mosquitto *mosq = NULL;
    int rc;
    size_t payload_len = PAYLOAD_LEN;
    char payload[PAYLOAD_LEN];
    int qos = true;
    int retain = 1;
    int mid;
    mosquitto_property *props = NULL;

    tw.tv_sec = opt->timeout / 1000;
    tw.tv_nsec = opt->timeout % 1000 * 1000000;

    printf(YEL"Connect to: http://%s:%d%s with timeout: %d\n"NRM, opt->host, opt->port, opt->uri,
           opt->timeout);

    sigemptyset(&sigset_block);
    sigaddset(&sigset_block, SIGPIPE);

    if (pthread_sigmask(SIG_BLOCK, &sigset_block, &sigset_restore) != 0)
    {
        fprintf(stderr, RED"Could not block signal\n"NRM);
        return NULL;
    }

    addr = get_sin(opt->host, opt->port);
    if (!addr)
    {
        fprintf(stderr, RED"Could not connect to: %s:%d\n"NRM, opt->host, opt->port);
        goto exit;
    }

    if (regcomp(&re_code, MATCH_CODE, REG_EXTENDED | REG_NEWLINE))
    {
        fprintf(stderr, RED"Could not compile regular expression\n"NRM);
        goto exit;
    }

    if (regcomp(&re_data, MATCH_DATA, REG_EXTENDED))
    {
        fprintf(stderr, RED"Could not compile regular expression\n"NRM);
        goto exit;
    }

    req = calloc(req_len, sizeof(char));
    if (!req)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        goto exit;
    }

    sprintf(req, GET_MESSAGE, opt->uri, opt->host);

    res = (char *)malloc(res_len);
    if (!req)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        goto exit;
    }

    pr = (params *)malloc(sizeof(params));
    if (!pr)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        goto exit;
    }

    opt->id = (char *)malloc(ID_LEN);
    if (!opt->id)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        goto exit;
    }
    snprintf((char *)opt->id, ID_LEN, "sensor%lu", pthread_self());

    mosq = mosquitto_new(opt->id, true, opt);
    if (!mosq)
    {
        fprintf(stderr, RED"Cannot create new client instance\n"NRM);
        goto exit;
    }
    //mosquitto_int_option(mosq, MOSQ_OPT_PROTOCOL_VERSION, opt->protocol_version);

    if (debug)
    {
        mosquitto_log_callback_set(mosq, log_callback);
    }

    mosquitto_connect_v5_callback_set(mosq, cb_connect);
    mosquitto_disconnect_v5_callback_set(mosq, cb_disconnect);
    mosquitto_publish_v5_callback_set(mosq, cb_publish);

    mosquitto_loop_start(mosq);

    rc = mosquitto_connect(mosq, opt->mqtt_host, opt->mqtt_port, opt->mqtt_keepalive);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, RED"Cannot connect to mqtt\n"NRM);
        goto exit;
    }
    //main loop
    while (!exit_flag)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1)
        {
            fprintf(stderr, RED"Could not create socket\n"NRM);
            break;
        }

        while (!exit_flag && connect(fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in)) < 0)
        {
            sleep(CONN_ERR_TIMEOUT);
        }

        if (exit_flag)
        {
            if (fd)
                close(fd);

            fprintf(stderr, RED"Got exit flag\n"NRM);
            break;
        }

        if (debug)
        {
            printf("send: %s\n", req);
        }

        int sigpipe_pending = -1;
        if (sigpending(&sigset_pending) != -1)
        {
            sigpipe_pending = sigismember(&sigset_pending, SIGPIPE);
        }

        if (sigpipe_pending == -1)
        {
            pthread_sigmask(SIG_SETMASK, &sigset_restore, NULL);
            fprintf(stderr, RED"Could not block signal, it's pending\n"NRM);
            break;
        }

        if (send(fd, req, req_len, 0) < 0)
        {
            fprintf(stderr, RED"Could not send to: %s:%d errono: %d\n"NRM, opt->host, opt->port, errno);
            if (errno == EPIPE && sigpipe_pending == 0)
            {
                ts.tv_sec = 0;
                ts.tv_nsec = 0;

                int sig;
                while ((sig = sigtimedwait(&sigset_block, 0, &ts)) == -1)
                {
                    if (errno != EINTR)
                        break;
                }
            }
            goto err;
        }

        memset(res, 0, res_len);
        if (recv(fd, res, res_len, 0) < 0)
        {
            fprintf(stderr, RED"Could not recv from: %s:%d errno: %d\n"NRM, opt->host, opt->port, errno);
            goto err;
        }

        if (debug)
        {
            printf(GRN"recv: %s\n"NRM, res);
        }

        if (regexec(&re_code, res, groups_code, groups_code_array, 0))
        {
            fprintf(stderr, RED"No matches code\n"NRM);
            goto err;
        }

        strncpy(code, res + groups_code_array[1].rm_so,
                groups_code_array[1].rm_eo - groups_code_array[1].rm_so);
        *(code + 3) = END;
        ucode = atoi(code);

        if (ucode != CODE_OK)
        {
            fprintf(stderr, RED"Wrong return code: %u\n"NRM, ucode);

            memset(payload, 0, PAYLOAD_LEN);
            payload_len = snprintf(payload, PAYLOAD_LEN, "Wrong return code: %u", ucode);
            rc = mosquitto_publish_v5(mosq, &mid, opt->log, payload_len, payload, qos, retain, props);
            if (rc != MOSQ_ERR_SUCCESS)
                mqtt_publish_error(rc);

            goto err;
        }

        if (regexec(&re_data, res, groups_data, groups_data_array, 0))
        {
            fprintf(stderr, RED"No matches data\n"NRM);
            goto err;
        }

        *pr->timestamp = END;
        *pr->temp = END;
        *pr->pressure = END;
        *pr->humidity = END;
        for (size_t i = 1; i < groups_data; i++)
        {
            data_len = groups_data_array[i].rm_eo - groups_data_array[i].rm_so;
            switch (i)
            {
                case 1:
                    if (data_len < sizeof(pr->timestamp))
                    {
                        strncpy(pr->timestamp, res + groups_data_array[i].rm_so, data_len);
                        *(pr->timestamp + data_len) = END;
                    }
                    else
                    {
                        fprintf(stderr, RED"Wrong data timestamp len: %lu\n"NRM, data_len);
                    }
                    break;
                case 2:
                    if (data_len < sizeof(pr->temp))
                    {
                        strncpy(pr->temp, res + groups_data_array[i].rm_so, data_len);
                        *(pr->temp + data_len) = END;
                    }
                    else
                    {
                        fprintf(stderr, RED"Wrong data temp len: %lu\n"NRM, data_len);
                    }
                    break;
                case 3:
                    if (data_len < sizeof(pr->pressure))
                    {
                        strncpy(pr->pressure, res + groups_data_array[i].rm_so, data_len);
                        *(pr->pressure + data_len) = END;
                    }
                    else
                    {
                        fprintf(stderr, RED"Wrong data pressure len: %lu\n"NRM, data_len);
                    }
                    break;
                case 4:
                    if (data_len < sizeof(pr->humidity))
                    {
                        strncpy(pr->humidity, res + groups_data_array[i].rm_so, data_len);
                        *(pr->humidity + data_len) = END;
                    }
                    else
                    {
                        fprintf(stderr, RED"Wrong data humitidy len: %lu\n"NRM, data_len);
                    }
                    break;
            }

            if (debug)
            {
                printf(GRN"Group %lu: %2u-%2u\n"NRM, i, groups_data_array[i].rm_so, groups_data_array[i].rm_eo);
            }

        }
        if (debug)
        {
            printf(GRN"\ntimestamp: %s\ntemp: %s\npressure: %s\nhumidity: %s\n\n"NRM,
                   pr->timestamp, pr->temp, pr->pressure, pr->humidity);
        }
        if (pr->timestamp && pr->temp)
        {
            memset(payload, 0, PAYLOAD_LEN);
            payload_len = snprintf(payload, PAYLOAD_LEN, PAYLOAD, pr->timestamp, pr->temp);
            rc = mosquitto_publish_v5(mosq, &mid, opt->topic, payload_len, payload, qos, retain, props);
            if (rc != MOSQ_ERR_SUCCESS)
                mqtt_publish_error(rc);
        }
err:
        close(fd);

        nanosleep(&tw, &tr);
    }//main loop
exit:
    pthread_sigmask(SIG_SETMASK, &sigset_restore, NULL);

    free(addr);
    free(pr);
    free(req);
    free(res);
    free((void *)opt->id);

    mosquitto_destroy(mosq);

    regfree(&re_data);
    regfree(&re_code);

    return NULL;
}

void help(const char *prog)
{
    printf(WHT
           "%s\n\t-a host or ip\n"
           "\t-p port\n"
           "\t-t timeout sensor quests in ms\n"
           "\t-b broker host\n"
           "\t-P broker port\n"
           "\t-d debug on\n"
           "\t-h print help\n"NRM, prog
          );
    exit(0);
}

int main(int argc, char *argv[])
{
    pthread_t threads[2];

    sensor_opt *opt = calloc(1, sizeof(sensor_opt));
    if (!opt)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        return 1;
    }

    int c;
    while ((c = getopt (argc, argv, "a:p:t:dhb:P:")) != -1)
        switch (c)
        {
            case 'a':
                opt->host = optarg;
                break;
            case 'p':
                opt->port = atoi(optarg);
                break;
            case 't':
                opt->timeout = atoi(optarg);
                break;
            case 'd':
                debug = 1;
                break;
            case 'b':
                opt->mqtt_host = optarg;
                break;
            case 'P':
                opt->mqtt_port = atoi(optarg);
                break;
            case 'h':
            default:
                free(opt);
                help(argv[0]);
        }

    if (!opt->host)
    {
        fprintf(stderr, RED"please set host(-a)\n"NRM);
        help(argv[0]);
    }

    if (opt->port == 0)
    {
        fprintf(stderr, RED"please set port(-p)\n"NRM);
        help(argv[0]);
    }

    if (opt->timeout == 0)
    {
        fprintf(stderr, RED"please set timeout(-t)\n"NRM);
        help(argv[0]);
    }

    if (!opt->mqtt_host)
    {
        fprintf(stderr, YEL"Use localhost for commection to broker\n"NRM);
        opt->mqtt_host = malloc(10);
        if (!opt->mqtt_host)
        {
            fprintf(stderr, RED"Cannot alloc\n"NRM);
            return 1;
        }
        snprintf((char *)opt->mqtt_host, 10, "127.0.0.1");
    }

    if (!opt->mqtt_port)
    {
        opt->mqtt_port = 1883;
        fprintf(stderr, YEL"Use port %d for commection to broker\n"NRM, opt->mqtt_port);
    }

    opt->mqtt_keepalive = 1;
    opt->mqtt_bind_address = malloc(10);
    if (!opt->mqtt_bind_address)
    {
        fprintf(stderr, RED"Cannot alloc\n"NRM);
        return 1;
    }

    snprintf((char *)opt->mqtt_bind_address, 10, "127.0.0.1");

    mosquitto_lib_init();

    opt->uri = ROOT;
    opt->topic = TOPIC;
    opt->log = TOPIC_LOG;

    if (pthread_create(&threads[0], NULL, sensor, (void *)opt))
    {
        fprintf(stderr, RED"Error creating thread\n"NRM);
        return 1;
    }
    sensor_opt *opt1 = calloc(1, sizeof(sensor_opt));
    if (!opt1)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        return 1;
    }

    memcpy(opt1, opt, sizeof(sensor_opt));
    opt1->uri = UNSTABLE;
    if (!opt1->mqtt_host)
    {
        fprintf(stderr, YEL"Use localhost for commection to broker\n"NRM);
        opt1->mqtt_host = malloc(10);
        if (!opt1->mqtt_host)
        {
            fprintf(stderr, RED"Cannot alloc\n"NRM);
            return 1;
        }
        snprintf((char *)opt1->mqtt_host, 10, "127.0.0.1");
    }

    if (!opt1->mqtt_port)
    {
        opt1->mqtt_port = 1883;
        fprintf(stderr, YEL"Use port %d for commection to broker\n"NRM, opt1->mqtt_port);
    }

    opt1->mqtt_keepalive = 1;
    opt1->mqtt_bind_address = malloc(10);
    if (!opt1->mqtt_bind_address)
    {
        fprintf(stderr, RED"Cannot alloc\n"NRM);
        return 1;
    }

    snprintf((char *)opt1->mqtt_bind_address, 10, "127.0.0.1");

    opt1->topic = TOPIC;
    opt1->log = TOPIC_LOG;

    if (pthread_create(&threads[1], NULL, sensor, (void *)opt1))
    {
        fprintf(stderr, RED"Error creating thread\n"NRM);
        return 1;
    }

    getc(stdin);
    exit_flag = 1;

    if (pthread_join(threads[0], NULL))
    {
        fprintf(stderr, RED"Error joining thread\n"NRM);
        return 2;
    }
    if (pthread_join(threads[1], NULL))
    {
        fprintf(stderr, RED"Error joining thread\n"NRM);
        return 2;
    }
    mosquitto_lib_cleanup();

    free(opt1);
    free(opt);

    return 0;
}
