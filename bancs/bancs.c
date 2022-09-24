/*
 *        B A N C S . C
 *
 * Initial created in 2022.09.
 *             Kylin Soong
 *
 * This is sample program for simulate Core Bank System.
 * 
 *  _______
 * |       |
 * |ESB(MQ)| 
 * |       |
 *  -------  
 *     | request
 *     |
 *     |
 *  -------  request       _______
 * |       |--------------|       |
 * | BANCS |              |  CARD |
 * |       |--------------|       |
 *  -------     response   -------
 *
 * 1. BANCS listen on 9805, 8805, CARD listen on 8806, BANCS connect to CARD via 8806(mark as connection 1), CARD connect to BANCS via 8805(mark as connection 2)
 * 2. A client simulate ESB send the request message to BANCS via 9902 which listened by BANCS
 * 2. BANCS forward request message to CARD
 * 3. CARD send response message to BANCS
 *   
 */

#include <stdio.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <signal.h>


#define MAX_DATA_LINE 4096

typedef void    Sigfunc(int);   /* for signal handlers */

struct sockaddr_storage esb_to_bancs, bancs_esb_ss, bancs_from_esb, bancs_card_ss, bancs_from_card, card_bancs_ss, card_from_bancs, bancs_to_card, card_to_bancs;

int domain, fromlen, bancs_from_esb_len, bancs_from_card_len, card_from_bancs_len;
int fd, fd_bancs, fd_to_card, fd_to_bancs;                 /* fd of network socket */
int connfd, connfd_bancs;

int buflen = 0;
int lazy = 2;

short port = 8805;
short cport = 8806;               /* TCP port number */
short inport = 9805; 

char *host;                     /* ptr to name of host */
int server = 1;                 /* 0=client, 1=server */

FILE * fp;
char bufr[MAX_DATA_LINE];
char *data;

extern int errno;
extern int optind;
extern char *optarg;

char Usage[] = "\
Usage: bancs -e [-options] <host of BANCS> \n\
       bancs -b [-options] <host of CARD>\n\
       bancs -c [-options] <host of BANCS>\n\
Common options:\n\
        -l ##   specify the message length\n\
        -p ##   port number to send to or listen at (default 8805/8806 9805)\n\
";

/* Socket */
int      Socket(int, int, int);
void     Bind(int, const struct sockaddr *, socklen_t);
void     Listen(int, int);
int      Accept(int, struct sockaddr *, socklen_t *);
void     Connect(int, const struct sockaddr *, socklen_t);
char     *Getpeername(int);

void     InboundHandler(void);

/* IO */
void     Close(int);
void     err_sys(const char *, ...);
void     out_sys(const char *, ...);
char     *Fgets(char *, int, FILE *);
void     Fputs(const char *, FILE *);
char     *concat(const char *s1, const char *s2);

pid_t    Fork(void);
void     sig_chld(int);
Sigfunc  *Signal(int, Sigfunc *);

void     Writen(int, void *, size_t);
ssize_t  writen(int, const void *, size_t);

ssize_t  Readline(int, void *, size_t);
ssize_t  readline(int, void *, size_t);
ssize_t  my_read(int fd, char *ptr);

/* Core logic entries*/
void     Str_puts(int);

int main(int argc, char **argv) 
{
    if (argc < 2) goto usage;
    
    pid_t              childpid;
    int c;

    while ((c = getopt(argc, argv, "ebcl:p:")) != -1) {
        switch (c) {
        case 'e':
            server = 0;
            break;
        case 'b':
            server = 1;
            break;
        case 'c':
            server = 2;
            break;
        case 'p':
            port = atoi(optarg);
            cport = port + 1;
            inport = port + 1000; 
            break;
        case 'l':
            buflen = atoi(optarg);
            break;
        default:
            goto usage;
        }
    }

    if (buflen <= 0) {
        goto usage;
    }

    if (optind == argc) {
        goto usage;
    }
    host = argv[optind];

  if(server == 0) {

    out_sys("start");

    if((fp = fopen("/etc/bancs.data","r")) != NULL) {
      while(fgets(bufr, MAX_DATA_LINE, fp) != NULL) {
        if (strncmp("#", bufr, strlen("#")) == 0 || strlen(bufr) < 3) 
          continue;

          char *key = strtok(bufr, "=");
          data = strtok(NULL, "=");
          key = strtok(key, "\r\t\n ");
          data = strtok(data, "\r\t\n ");

          if(data ==  NULL) 
            err_sys("data is null");
      }
    }

    struct addrinfo *dest;
    if(getaddrinfo(host, NULL, NULL, &dest)!=0) 
      err_sys("badhostname");
   
    memset(&esb_to_bancs, 0, sizeof(&esb_to_bancs));
    memcpy((void*)&esb_to_bancs,(void*)dest->ai_addr, dest->ai_addrlen);
    ((struct sockaddr_in *)&esb_to_bancs)->sin_port = htons(inport);

    fd = Socket(AF_INET, SOCK_STREAM, 0);
    out_sys("socket");

    Connect(fd, (struct sockaddr *)&esb_to_bancs, sizeof(esb_to_bancs));
    out_sys(concat("connect to bancs ", host));
    Writen(fd, data, strlen(data));
    out_sys("send message to bancs");
    out_sys(concat("message: ", data)); 
    out_sys("exit"); 

  } else if(server == 1) {

    out_sys("start");

    Signal(SIGCHLD, sig_chld);
    int rc = fork();
    if (rc < 0 ) {
      err_sys("Error:unable to create thread");
    } else if ( rc == 0) {
      InboundHandler();
    } else {

       sleep(lazy);      

       // Standard Sock wait for CARD to connect,
       // and handle CARD's response message
       memset(&bancs_card_ss, 0, sizeof(&bancs_card_ss));
       memset(&bancs_from_card, 0, sizeof(&bancs_from_card));
       
       ((struct sockaddr_in *)&bancs_card_ss)->sin_port = htons(port);
       fd_bancs = Socket(AF_INET, SOCK_STREAM, 0);
       out_sys("socket");

       Bind(fd_bancs, (struct sockaddr *)&bancs_card_ss, sizeof(bancs_card_ss));
       out_sys("bind");

       Listen(fd_bancs, 5);
       out_sys("listen");

       connfd_bancs = Accept(fd_bancs, (struct sockaddr *)&bancs_from_card, &bancs_from_card_len);
       char *peer = Getpeername(connfd_bancs);
       out_sys(concat("from card: ", peer));

       // Handle CARD system's response message;
       // Repeatedly extract the message
       // Current handling mechanism is only output the response message to log.
       for(;;) {
          char    line[MAX_DATA_LINE];
          ssize_t n = Readline(connfd_bancs, line, buflen);
          if(n < 0) {
              err_sys("read");
          } else if (n == 0) {
              out_sys(concat("connection closed by ", peer));
              return;
          }

          out_sys("response message from CARD");
          out_sys(line);
        }
     } 

  } else if(server == 2) {

    out_sys("start");

    memset(&card_bancs_ss, 0, sizeof(&card_bancs_ss));
    memset(&card_from_bancs, 0, sizeof(&card_from_bancs));

    ((struct sockaddr_in *)&card_bancs_ss)->sin_port = htons(cport);
    fd = Socket(AF_INET, SOCK_STREAM, 0);
    out_sys("socket");

    Bind(fd, (struct sockaddr *)&card_bancs_ss, sizeof(card_bancs_ss));
    out_sys("bind");

    Listen(fd, 5);
    out_sys("listen");

    connfd = Accept(fd, (struct sockaddr *)&card_from_bancs, &card_from_bancs_len);
    char *peer = Getpeername(connfd);
    out_sys(concat("from bancs ", peer));

    sleep(lazy);   

    // connect to bancs
    struct addrinfo *dest;
    if(getaddrinfo(host, NULL, NULL, &dest)!=0)
      err_sys("badhostname");

    memset(&card_to_bancs, 0, sizeof(&card_to_bancs));
    memcpy((void*)&card_to_bancs,(void*)dest->ai_addr, dest->ai_addrlen);

    ((struct sockaddr_in *)&card_to_bancs)->sin_port = htons(port);

    fd_to_bancs = Socket(AF_INET, SOCK_STREAM, 0);
    out_sys("socket");

    Connect(fd_to_bancs, (struct sockaddr *)&card_to_bancs, sizeof(card_to_bancs));
    out_sys(concat("connect to bancs ", host));

    // handle bancs request message
    for(;;) {
      char    line[buflen];
      ssize_t n = Readline(connfd, line, buflen);
      if(n < 0) {
        err_sys("read");
      } else if (n == 0) {
        out_sys(concat("connection closed by ", peer));
        return;
      }

      Writen(fd_to_bancs, line, strlen(line));
      out_sys("response message to bancs");
      out_sys(concat("message: ", line));   
    }

  }

  return 1;

  usage:
    fprintf(stderr,Usage);
    exit(1);
}

/**
 * Handle the client request 
 */
void InboundHandler() {

  out_sys("inbound handler start");
  
  struct addrinfo *dest;
  if(getaddrinfo(host, NULL, NULL, &dest)!=0)
    err_sys("badhostname");

  memset(&bancs_to_card, 0, sizeof(&bancs_to_card));
  memcpy((void*)&bancs_to_card,(void*)dest->ai_addr, dest->ai_addrlen);

  ((struct sockaddr_in *)&bancs_to_card)->sin_port = htons(cport);

  fd_to_card = Socket(AF_INET, SOCK_STREAM, 0);
  out_sys("socket");

  Connect(fd_to_card, (struct sockaddr *)&bancs_to_card, sizeof(bancs_to_card));
  out_sys(concat("connect to card ", host));

  memset(&bancs_esb_ss, 0, sizeof(&bancs_esb_ss));
  memset(&bancs_from_esb, 0, sizeof(&bancs_from_esb));

  ((struct sockaddr_in *)&bancs_esb_ss)->sin_port = htons(inport);
  fd = Socket(AF_INET, SOCK_STREAM, 0);
  out_sys("inbound handler socket");

  Bind(fd, (struct sockaddr *)&bancs_esb_ss, sizeof(bancs_esb_ss));
  out_sys("inbound handler bind");

  Listen(fd, 5);
  out_sys("inbound handler listen");

  for(;;) {
    connfd = Accept(fd, (struct sockaddr *)&bancs_from_esb, &bancs_from_esb_len);
    char *peer = Getpeername(connfd);
    out_sys(concat("inbound message from ", peer));

    char    line[buflen];
    ssize_t n = Readline(connfd, line, buflen);
    if(n < 0) {
      err_sys("read");
    } else if (n == 0) {
      out_sys(concat("connection closed by ", peer));
      return;
    }

    Writen(fd_to_card, line, strlen(line));
    out_sys("request message to card");
    out_sys(concat("message: ", line));

    Close(connfd);
  }
}

int Socket(int family, int type, int protocol)
{
    int listen_sock;

    if ( (listen_sock = socket(family, type, protocol)) < 0)
        err_sys("socket");

    return (listen_sock);
}

void Bind(int fd, const struct sockaddr *sa, socklen_t salen)
{
    if (bind(fd, sa, salen) < 0)
        err_sys("bind");
}

void Connect(int fd, const struct sockaddr *sa, socklen_t salen)
{
    if (connect(fd, sa, salen) < 0)
        err_sys("connect");
}

void Listen(int fd, int backlog)
{
    char *ptr;

    if ((ptr = getenv("LISTENQ")) != NULL) { /*4can override 2nd argument with environment variable */
        backlog = atoi(ptr);
    }

    if (listen(fd, backlog) < 0)
        err_sys("listen");
}

int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr)
{
    int sock;

    if ((sock = accept(fd, sa, salenptr)) < 0)
        err_sys("accept");

    return (sock);
}

char *Getpeername(int connfd)
{
    char      buf[300];
    char      namebuf[256];
    in_port_t port;
 
    struct sockaddr_storage peer;
    int peerlen = sizeof(peer);

    if (getpeername(connfd, (struct sockaddr *) &peer, &peerlen) < 0) {
        err_sys("getpeername");
    }

    switch(peer.ss_family) {
    case AF_INET:
        inet_ntop(peer.ss_family, &((struct sockaddr_in *)&peer)->sin_addr, namebuf, 256);
        port = ntohs(((struct sockaddr_in *)&peer)->sin_port);
        break;
    case AF_INET6:
        inet_ntop(peer.ss_family, &((struct sockaddr_in6 *)&peer)->sin6_addr, namebuf, 256);
        port = ntohs(((struct sockaddr_in6 *)&peer)->sin6_port);
        break;
    }

    snprintf(buf, sizeof(buf), "%s:%u", namebuf, port);
    char *result = malloc(strlen(buf) +1);
    strcpy(result, buf);

    return result;
}

void Close(int fd)
{
    if (close(fd) == -1)
        err_sys("close");
}

void err_sys(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    char* role = "CLIENT";
    if(server == 0)
        role = "CLIENT";
    else if (server == 1)
        role = "BANCS";
    else if (server == 2)
        role = "CARD";

    //perror(fmt);
    fprintf(stderr,"%s error, errno=%d, msg=%s\n", role, errno, fmt);
    va_end(ap);

    exit(0);
}

void out_sys(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    char* role = "CLIENT";
    if(server == 0)
        role = "CLIENT";
    else if (server == 1)
        role = "BANCS";
    else if (server == 2)
        role = "CARD";

    char cur_time[128];
    struct tm*  ptm;
    time_t now = time(NULL);
    ptm = localtime(&now);
    strftime(cur_time, 128, "%d-%b-%Y %H:%M:%S", ptm);

    printf("%s (%d) %s: %s\n", cur_time, getpid(), role, fmt);
    va_end(ap);
}

char *Fgets(char *ptr, int n, FILE *stream)
{
    char    *rptr;

    if ( (rptr = fgets(ptr, n, stream)) == NULL && ferror(stream))
        err_sys("fgets");

    return (rptr);
}

void Fputs(const char *ptr, FILE *stream)
{
    if (fputs(ptr, stream) == EOF)
        err_sys("fputs");
}

char *concat(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1) + strlen(s2) + 1);
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}


void sig_chld(int signo)
{
    pid_t   pid;
    int     stat;

    while ( (pid = waitpid(-1, &stat, WNOHANG)) > 0) {
       char buf[30];
       snprintf(buf, sizeof(buf), "child %d terminated", pid);    
       out_sys(buf);
    }
    
    return;
}

pid_t Fork(void)
{
    pid_t pid;

    if ((pid = fork()) == -1)
        err_sys("fork");
    return (pid);
}

Sigfunc * Signal(int signo, Sigfunc *func)        /* for our signal() function */
{
    Sigfunc *sigfunc;

    if ( (sigfunc = signal(signo, func)) == SIG_ERR)
            err_sys("signal");
    return(sigfunc);
}

void Writen(int fd, void *ptr, size_t nbytes){

    if (writen(fd, ptr, nbytes) != nbytes)
        err_sys("writen");
}

ssize_t writen(int fd, const void *vptr, size_t n)
{
    size_t     nleft;
    ssize_t    nwritten;
    const char *ptr;

    ptr = vptr;
    nleft = n;

    while (nleft > 0) {
        if ((nwritten = write(fd, ptr, nleft)) <= 0) {
            if (nwritten < 0 && errno == EINTR)
                nwritten = 0;
            else
                return (-1);
        }
        nleft -= nwritten;
        ptr   += nwritten;
    }

    return (n);
}

ssize_t Readline(int fd, void *ptr, size_t maxlen)
{
    ssize_t n;

    if ( (n = readline(fd, ptr, maxlen)) < 0)
        err_sys("readline");
    return (n);
}

ssize_t readline(int fd, void *vptr, size_t maxlen)
{
    ssize_t n, rc;
    char    c, *ptr;
    char    read_buf[MAX_DATA_LINE];

    ptr = vptr;
    for (n = 0; n < maxlen; n++) {
        if ( (rc = my_read(fd, &c)) == 1) {
            *ptr++ = c;
            if (c == '\n')
                break;
        } else if (rc == 0) {
            *ptr = 0;
            return(n - 1);
        } else {
            return(-1);
        }
    }

    *ptr = 0;
    return(n);
}

ssize_t my_read(int fd, char *ptr)
{

    int      read_cnt;
    char     *read_ptr;
    char     read_buf[MAX_DATA_LINE];

    if (read_cnt <= 0) {
        again:
        if ( (read_cnt = read(fd, read_buf, sizeof(read_buf))) < 0) {
            if (errno == EINTR)
                goto again;
        } else if (read_cnt == 0) {
            return(0);
        }

        read_ptr = read_buf;
    }

    read_cnt--;
    *ptr = *read_ptr++;
    return(1);
}

void Str_puts(int sockfd)
{
    char recvline[MAX_DATA_LINE];

    if (Readline(sockfd, recvline, MAX_DATA_LINE) == 0)
        err_sys("server terminated prematurely");
    
    Fputs(recvline, stdout);
} 