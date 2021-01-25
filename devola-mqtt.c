#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <mosquitto.h>
#include <mqtt_protocol.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/wireless.h>

#define TOPICSFILE "topics.conf"

#define NETWORK "eth0"
#define HOST "192.168.1.1"
#define PORT 1883
#define KEEPALIVE 60

#define INPUTSUBTOPIC "tele/%s/RESULT"
#define INPUTSUBTOPICSCAN "tele/%[^/]/RESULT"
#define INPUTPATTERN "tele/+/RESULT"
#define INPUTPUBTOPIC "cmnd/%s/SSERIALSEND5"
#define INPUTPOWERTOPIC "cmnd/%s/POWER"
#define INPUTHEXSTR "0210%02x%02x02001900%02x%02x%04x%02x01000001"

#define OUTPUTSUBTOPIC "cmnd/%s/%s"
#define OUTPUTSUBTOPICSCAN "cmnd/%[^/]/%s"
#define OUTPUTPATTERN "cmnd/+/+"
#define OUTPUTPUBTOPIC "stat/%s/%s"

enum command
{
    POWER = 0,
    CHILDLOCK = 1,
    MODE = 2,
    SETPOINT = 3,
    TEMP = 4,
    TIMER = 5
};
char *cmdmap[] = {"POWER", "CHILDLOCK", "MODE", "SETPOINT", "TEMP", "TIMER"};
#define NUMCMDS 6

enum onoff
{
    OFF = 2,
    ON = 1
};

struct devolastate {
    enum onoff power;
    enum onoff childlock;
    int mode;
    int setpoint;
    int timer;
    int temp;
};

/* Topicmap linked list of input(raw) device topic and output(processed) device topic */
struct topicmap {
    char *input;
    char *output;
    struct devolastate state;
    struct topicmap *next;
};

int cmdhash(char *cmd)
{
    int i;

    for (i = 0; i < NUMCMDS; i++)
        if (strcmp(cmd, cmdmap[i]) == 0)
            return (i);

    return (-1);
}

char *hexcmd(char *hexstr)
{
    char tempstr[80];
    int i, checksum = 0;

    for (i = 0; i < strlen(hexstr); i+=2){
        char byte[3];

        byte[0] = hexstr[i]; byte[1] = hexstr[i+1]; byte[2] = '\0';
        checksum = checksum + strtoul(byte, NULL, 16);
    }

    sprintf(tempstr, "F1F1%s%02X7E", hexstr, checksum & 0xff);

    return (strcpy(hexstr, tempstr));
}

void wait_for_network()
{
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;

        strcpy(ifr.ifr_name, NETWORK);

        fprintf(stderr, "Waiting for network\n");

        while (true) {
                if (ioctl(sockfd, SIOCGIFADDR, &ifr) == 0) {
                        ioctl(sockfd, SIOCGIFFLAGS, &ifr);
                        fprintf(stderr, "Flags: %x\n", ifr.ifr_flags);
                        if (ifr.ifr_flags & IFF_RUNNING)
                                break;
                }

                sleep(15);
        }

        fprintf(stderr, "Network connection now up\n");
}

struct topicmap *find_in_topicmap(struct topicmap *topicmap, char *inputtopic, char *outputtopic)
{
    assert(inputtopic != outputtopic); /* Can't search for both at once */

    if (inputtopic) {
        while (strcmp(inputtopic, topicmap->input)){
            if (topicmap->next == NULL) { /* Hit end of linked list and no match */
                fprintf(stderr, "Hit end of list and couldn't find input topic: %s\n", inputtopic);
                abort();
            }
            else topicmap = topicmap->next;
        }
    }
    else {
        while (strcmp(outputtopic, topicmap->output)){
            if (topicmap->next == NULL) {
                fprintf(stderr, "Hit end of list and couldn't find output topic: %s\n", outputtopic);
                abort();
            }
            else
                topicmap = topicmap->next;
        }
    }

    return (topicmap);
}


void my_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
        /* Print all log messages regardless of level. */
        fprintf(stderr, "%s\n", str);
}

void my_connect_callback(struct mosquitto *mosq, void *userdata, int result)
{
    struct topicmap *ptr;

    if (!result)
    {
        for (ptr = userdata; ptr != NULL; ptr = ptr->next) {
            char buf[256];
            int i, retval;

            sprintf(buf, INPUTSUBTOPIC, ptr->input);
            retval = mosquitto_subscribe(mosq, NULL, buf, 1);

            for (i = 0; i < NUMCMDS; i++) {
                sprintf(buf, OUTPUTSUBTOPIC, ptr->output, cmdmap[i]);
                mosquitto_subscribe(mosq, NULL, buf, 1);
            }
        } 
    }
}

void my_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    bool result;
    struct topicmap *topicmap;
    char pubtopic[80];

#ifdef DEBUG
    fprintf(stderr, "message->payload: %s\n", message->payload);
#endif

    mosquitto_topic_matches_sub(INPUTPATTERN, message->topic, &result);
    if (result) {
        /* Process payload from raw (input) device */
        char inputtopic[80];       

        if (sscanf(message->topic, INPUTSUBTOPICSCAN, inputtopic) != 1){
            fprintf(stderr, "Couldn't find Tasmota input topic in subscribed topic: %s\n", message->topic);
            abort();
        }
        else {
            enum onoff power, childlock;
            int mode;
            unsigned int setpoint, timer, temp;
            int numvars = -1;

            topicmap = find_in_topicmap(userdata, inputtopic, NULL);

            if ((numvars = sscanf(message->payload, "{\"SSerialReceived\":\"F2F2"INPUTHEXSTR"%*2x7E\"}", 
                                        &power, &childlock, &mode, &setpoint, &timer, &temp)) == 6) {
                if (topicmap->state.power != power) {
                    char pwrstr[4];

                    topicmap->state.power = power;
                    strcpy(pwrstr, (power == ON) ? "ON" : "OFF");
                    sprintf(pubtopic, OUTPUTPUBTOPIC, topicmap->output, "POWER");
                    mosquitto_publish(mosq, NULL, pubtopic, strlen(pwrstr), pwrstr, 1, false);
                }
                if (power == ON) {
                    char buf[4];

                    topicmap->state.temp = temp;
                    sprintf(buf, "%2d", temp);
                    sprintf(pubtopic, OUTPUTPUBTOPIC, topicmap->output, "TEMP");
                    mosquitto_publish(mosq, NULL, pubtopic, strlen(buf), buf, 1, false);

                    strcpy(buf, (childlock == ON) ? "ON" : "OFF");
                    topicmap->state.childlock = childlock;
                    sprintf(pubtopic, OUTPUTPUBTOPIC, topicmap->output, "CHILDLOCK");
                    mosquitto_publish(mosq, NULL, pubtopic, strlen(buf), buf, 1, false);

                    topicmap->state.mode = mode;
                    sprintf(buf, "%2u", (unsigned int)mode);
                    sprintf(pubtopic, OUTPUTPUBTOPIC, topicmap->output, "MODE");
                    mosquitto_publish(mosq, NULL, pubtopic, strlen(buf), buf, 1, false);

                    topicmap->state.setpoint = setpoint;
                    sprintf(buf, "%2u", setpoint);
                    sprintf(pubtopic, OUTPUTPUBTOPIC, topicmap->output, "SETPOINT");
                    mosquitto_publish(mosq, NULL, pubtopic, strlen(buf), buf, 1, false);

                    timer = (timer & 0xFF) - 1;
                    topicmap->state.timer = timer;
                    sprintf(buf, "%d", timer);
                    sprintf(pubtopic, OUTPUTPUBTOPIC, topicmap->output, "TIMER");
                    mosquitto_publish(mosq, NULL, pubtopic, strlen(buf), buf, 1, false);
                }
            }
            else {
                fprintf(stderr, "Couldn't unpack input payload: %s - only %d parameters\n", message->payload, numvars);
                abort();
            }
        }
    } else {
        mosquitto_topic_matches_sub(OUTPUTPATTERN, message->topic, &result);

        if (result) {
            /* Process payload from processed (output) device ie HA */
            char outputtopic[80], cmnd[80];       

            if (sscanf(message->topic, OUTPUTSUBTOPICSCAN, outputtopic, cmnd) != 2){
                fprintf(stderr, "Couldn't find Tasmota output topic and cmnd in subscribed topic: %s\n", message->topic);
                abort();
            }
            else {
                char payload[80];

                topicmap = find_in_topicmap(userdata, NULL, outputtopic);

                if (((char *)(message->payload) == NULL) || (*(char*)(message->payload) == '\0'))
                    fprintf(stderr, "Received null payload for topic: %s\n", message->topic);
                else {
                    if (cmdhash(cmnd) == POWER){
                        sprintf(pubtopic, INPUTPOWERTOPIC, topicmap->input);
                        mosquitto_publish(mosq, NULL, pubtopic, strlen(message->payload), message->payload, 1, false);
                    }
                    else if (topicmap->state.power == OFF)
                        fprintf(stderr, "Can't set heater controls while power state is off\n");
                    else {
                        switch (cmdhash(cmnd))
                        {
                        case CHILDLOCK:
                            sprintf(payload, INPUTHEXSTR, 0, (strcmp(message->payload, "ON") == 0) ? 1 : 2, 0, 0, 0, 0);
                            break;
                        case MODE:
                            sprintf(payload, INPUTHEXSTR, 0, 0, strtol(message->payload, NULL, 10), 0, 0, 0);
                            break;
                        case SETPOINT:
                            sprintf(payload, INPUTHEXSTR, 0, 0, 0, strtol(message->payload, NULL, 10), 0, 0);
                            break;
                        case TIMER:
                            sprintf(payload, INPUTHEXSTR, 0, 0, 0, 0, strtol(message->payload, NULL, 10) + 1, 0);
                            break;
                        }
                        sprintf(pubtopic, INPUTPUBTOPIC, topicmap->input);
                        mosquitto_publish(mosq, NULL, pubtopic, 42, hexcmd(payload), 1, false);
                    }
                }
            }
        }
        else { 
            /* error */
            perror("mosquitto_topic_matches_sub");
            abort();
        }
    }
}

int main(int argc, char *argv[]) {
    struct mosquitto *mosq = NULL;
    struct topicmap topicmap;
    struct topicmap *tmptr = &topicmap, *lasttmptr = &topicmap ; 
    
    {    /* Read config file to get list of topics */
        FILE *fp;
        char *linebuf = NULL;
        size_t len = 0;
 
        if ((fp = fopen(TOPICSFILE, "r")) == 0) {
            perror("fopen");
            abort();
        }

        bzero(&topicmap, sizeof(topicmap));

        while (true) {
            char iptopicbuf[256], optopicbuf[256];

            if (getline(&linebuf, &len, fp) == -1) {
                lasttmptr->next = NULL;
                break;
            }

            if (linebuf[0] == '#' || linebuf[0] == '\n')
                continue;

            if (sscanf(linebuf, "%s %s", iptopicbuf, optopicbuf) != 2) {
                fprintf(stderr, "Badly formed line in topics.conf: %s\n", linebuf);
                abort();
            }
            
            if ((tmptr->input = strdup(iptopicbuf)) == NULL)
            {
                perror("strdup ip");
                abort();
            }
            if ((tmptr->output = strdup(optopicbuf)) == NULL)
            {
                perror("strdup op");
                abort();
            }
            if ((tmptr->next = malloc(sizeof(struct topicmap))) == NULL)
            {
                perror("malloc");
                abort();
            }

            bzero(tmptr->next, sizeof(struct topicmap));

            lasttmptr = tmptr;
            tmptr = tmptr->next;
        }

        free(linebuf);
        fclose(fp);
    }

    assert(lasttmptr != tmptr); /* No topic lines read */

    wait_for_network();

    /* Initialise mosquitto */
    mosquitto_lib_init();
    mosq = mosquitto_new(NULL, true, &topicmap);

    if(!mosq){
        fprintf(stderr, "Error: %u - %s.\n", errno, strerror(errno));
        abort();
    }

    mosquitto_message_callback_set(mosq, my_message_callback);
    mosquitto_log_callback_set(mosq, my_log_callback);
    mosquitto_connect_callback_set(mosq, my_connect_callback);

    if (mosquitto_connect(mosq, HOST, PORT, KEEPALIVE)){
        fprintf(stderr, "Unable to connect: %u - %s\n", errno, strerror(errno));
        abort();
    }

    mosquitto_loop_forever(mosq, -1, 1);
}