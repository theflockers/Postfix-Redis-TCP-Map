/**
 *  Postfix Redis tcp map is a daemon to implement Postfixs tcp lookup table.
 *  Copyright (C) 2011  Leandro Mendes <theflockers at gmail dot com>
 *  ---------------------------------------------------------------------------
 *  This file is part of postfix-redis-tcp-map.
 *
 *  postfix-redis-tcp-map is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  postfix-redis-tcp-map is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with postfix-redis-tcp-map.  If not, see <https://www.gnu.org/licenses/>.
 */
 
/**
 * @file main.c
 * @brief Main file for postfix-redis tcp-daemon
 * @author Leandro Mendes <theflockers at gmail dot com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <sysexits.h>
#include <syslog.h>

#ifdef USE_EPOLL
#include <epoll.h>
#endif

/* libevent */
#include <event.h>

#include "tcp_mapper.h"

#define BACKLOG     1024
#define MAX_EVENTS  65550

/* configuration file */
#include "config.h"
config parseConfig(char *filename);
config cfg;

/* redis */
#include "hiredis.h"
redisPool redis_pool;

#ifdef HAS_MYSQL
#include <mysql.h>
MYSQL     *mysql;

/**
 * @name init_mysql
 * @description initiates a mysql instance
 * @return MYSQL *mysql
 */
MYSQL * init_mysql(void);
#endif

#ifdef HAS_PGSQL	
#include <libpq-fe.h>
PGconn    *pgsql;

/**
 * @name init_pgsql
 * @description initiates a pgsql instance
 * @return PGconn *pgsql
 */
PGconn * init_pgsql(void);
#endif

#ifdef HAS_LDAP
#include <ldap.h>
LDAP *ldap;

/**
 * @name init_ldap
 * @description initiates a ldap instance
 * @return LDAP *ldap
 */
LDAP * init_ldap(void);
#endif

/* others */
int init_time;

/* display help */
void help(void){
    printf("Usage: postfix-redis [ params ]\n\n \
            -c filepath\033[30GLoad configuration file\n \
            -h\033[30GShow this help\n\n");
    exit(0);
}

/**
 * @name signal_handler
 * @description catch some signals 
 * @params int sig
 */
void signal_handler(int sig) {
    switch(sig) {
        case SIGTERM:
        case SIGHUP:
        case SIGINT:
            event_loopbreak();
            break;
        default:
            printf("Unhandled signal\n");
            break;
    }
}

/**
 * @name handler_request
 * @description connection handler
 * @params int s
 * @params int fd
 */
int handle_request(int s, int fd);

/**
 * @name setnonblocking
 * @description Sets the socket to a non blocking socket
 * @params int fd
 */
int setnonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return 0;
}

/**
 * @name main
 * @description main function
 * @params int argc
 * @params char **argv
 */
int main(int argc, char **argv) {

    int    sock;
    int    optval = 1;
    struct protoent *proto;
    struct sockaddr_in listen_addr;
    char   config_file[255] = "";

    /* The socket accept event. */
    struct event ev_accept;

    /* Initialize libevent. */
    event_init();
    
    pid_t  pid;
    time_t now;


    /* Parse the command line */
    int i;
    for(i=0; i<sizeof(argv); i++) {
        if(argv[i] == NULL)
            break;

        /* config file */
        if(strncmp(argv[i], "-c", 3) == 0)
            snprintf(config_file, strlen(argv[i + 1])+1, "%s", argv[i + 1]);

        /* help */
        if(strncmp(argv[i], "-h", 3) == 0)
            help();

        /* daemonize */
        if(strncmp(argv[i], "-d", 3) == 0) {
            pid  = fork();
            if(pid > 0) {
                exit(0);
            }
        }

    }

    /* opens the kernel logger */
    openlog("postfix-redis", LOG_PID, LOG_MAIL);

    /* parse the config file */
    cfg = parseConfig(config_file);

#ifdef HAS_MYSQL
    /* initiates mysql */
    if(!cfg.mysql_enabled == 0) {
        mysql = init_mysql();
    }
#endif

#ifdef HAS_PGSQL
    /* initiates pgsql */
    if(!cfg.pgsql_enabled == 0) {
        pgsql = init_pgsql();
    }
#endif

#ifdef HAS_LDAP
    /* initiates ldap */
    if(!cfg.ldap_enabled == 0) {
        ldap = init_ldap();
    }
#endif

    proto = getprotobyname("tcp");
    sock = socket(AF_INET, SOCK_STREAM, proto->p_proto);
    
    memset(&listen_addr, 0, sizeof(struct sockaddr));

    listen_addr.sin_family      = AF_INET;
    listen_addr.sin_port        = htons(cfg.listen_port);
    listen_addr.sin_addr.s_addr = inet_addr(cfg.listen_address);

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, 
            sizeof(int));

    if(bind(sock, (struct sockaddr *)&listen_addr, 
            sizeof(listen_addr)) != 0) {
        perror("Error binding socket.");
        syslog(LOG_INFO, "Error binding socket");
        exit(1);
    }
    if(listen(sock, 1024) != 0) {
        perror("Could not listen socket");
        syslog(LOG_INFO, "Could not listen socket");
        exit(-1);
    }

    if(setnonblocking(sock) < 0) {
        perror("Error to set socket non block");
        syslog(LOG_INFO, "Error to set socket non block");
        exit(1);
    }

    memset(&redis_pool, 0, sizeof(redisPool));
    if(redisPoolInit(&redis_pool, cfg.redis_address, REDIS_POOL_SIZE) == -1) {
        printf("could not init postfix-redis\n");
        syslog(LOG_INFO, "could not init postfix-redis");
        exit(-1);
    }

    time(&now);

    init_time = (int) now;

	event_set(&ev_accept, sock, EV_READ|EV_PERSIST, on_accept, NULL);
	event_add(&ev_accept, NULL);

	/* Start the libevent event loop. */
	event_dispatch();

    return 0;
}
