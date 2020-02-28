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

#include <mqtt_protocol.h>
#include <mosquitto.h>

#define CONN_ERR_TIMEOUT 1
#define RECV_LEN 1024
#define ID_LEN 256
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
    const char *id;
    const char *mqtt_host;
    int mqtt_port;
    int mqtt_qos;
    int mqtt_opts;
    int mqtt_keepalive;
    int topics_count;
    char **topics;
} sensor_opt;

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
        mosquitto_subscribe_multiple(mosq, NULL, opt->topics_count, opt->topics, opt->mqtt_qos,
                                     opt->mqtt_opts, NULL);
    }
}

void cb_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
    (void)mosq;
    sensor_opt *opt = (sensor_opt *)obj;

    if (debug)
    {
        fprintf(stderr, YEL"Subscribed to: %s:%d\n"NRM, opt->mqtt_host, opt->mqtt_port);
    }
}

void cb_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message,
                const mosquitto_property *properties)
{
    (void)mosq;
    (void)obj;
    (void)properties;

    fprintf(stdout, GRN"%s\n"NRM,(const char*)message->payload);
}

void *subscriber(void *data)
{
    sensor_opt *opt = (sensor_opt *)data;
    struct mosquitto *mosq = NULL;
    int rc;

    printf(YEL"Connect to: %s:%d\n"NRM, opt->mqtt_host, opt->mqtt_port);

    opt->id = (char *)malloc(ID_LEN);
    if (!opt->id)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        goto exit;
    }
    snprintf((char *)opt->id, ID_LEN, "logger%lu", pthread_self());

    mosq = mosquitto_new(opt->id, true, opt);
    if (!mosq)
    {
        fprintf(stderr, RED"Cannot create new client instance\n"NRM);
        goto exit;
    }

    if (debug)
    {
        mosquitto_log_callback_set(mosq, log_callback);
    }

    mosquitto_connect_v5_callback_set(mosq, cb_connect);
    mosquitto_disconnect_v5_callback_set(mosq, cb_disconnect);
    mosquitto_subscribe_callback_set(mosq, cb_subscribe);
    mosquitto_message_v5_callback_set(mosq, cb_message);

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
        sleep(1);
    }//main loop
exit:

    free((void *)opt->id);

    mosquitto_destroy(mosq);

    return NULL;
}

void help(const char *prog)
{
    printf(WHT
           "%s\n\t-b broker host\n"
           "\t-p broker port\n"
           "\t-d debug on\n"
           "\t-h print help\n"NRM, prog
          );
    exit(0);
}

int main(int argc, char *argv[])
{
    pthread_t threads[1];

    sensor_opt *opt = calloc(1, sizeof(sensor_opt));
    if (!opt)
    {
        fprintf(stderr, RED"Could not alloc\n"NRM);
        return 1;
    }

    int c;
    while ((c = getopt (argc, argv, "p:dhb:")) != -1)
        switch (c)
        {
            case 'p':
                opt->mqtt_port = atoi(optarg);
                break;
            case 'd':
                debug = 1;
                break;
            case 'b':
                opt->mqtt_host = optarg;
                break;
            case 'h':
            default:
                free(opt);
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

    opt->topics_count = 2;
    opt->topics = calloc(opt->topics_count, sizeof(const char *));
    opt->topics[0] = strdup(TOPIC);
    opt->topics[1] = strdup(TOPIC_LOG);

    mosquitto_lib_init();

    if (pthread_create(&threads[0], NULL, subscriber, (void *)opt))
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
    mosquitto_lib_cleanup();

    for (int i = 0; i < opt->topics_count; i++)
        free(opt->topics[i]);

    free(opt->topics);
    free(opt);

    return 0;
}
