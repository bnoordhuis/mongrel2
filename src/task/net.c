#include "taskimpl.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/poll.h>

int
netannounce(int istcp, char *server, int port)
{
    int fd = 0, n = 0, proto = 0;
    struct sockaddr_in sa;
    socklen_t sn;
    uint32_t ip;

    taskstate("netannounce");
    proto = istcp ? SOCK_STREAM : SOCK_DGRAM;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    if(server != nil && strcmp(server, "*") != 0){
        if(netlookup(server, &ip) < 0){
            taskstate("netlookup failed");
            return -1;
        }
        memmove(&sa.sin_addr, &ip, 4);
    }
    sa.sin_port = htons(port);
    if((fd = socket(AF_INET, proto, 0)) < 0){
        taskstate("socket failed");
        return -1;
    }
    
    /* set reuse flag for tcp */
    sn = sizeof(n);
    if(istcp && getsockopt(fd, SOL_SOCKET, SO_TYPE, (void*)&n, &sn) >= 0){
        n = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&n, sizeof n);
    }

    if(bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0){
        taskstate("bind failed");
        fdclose(fd);
        return -1;
    }

    if(proto == SOCK_STREAM)
        listen(fd, 16);

    fdnoblock(fd);
    taskstate("netannounce succeeded");
    return fd;
}

int
netaccept(int fd, char *server, int *port)
{
    int cfd, one;
    struct sockaddr_in sa;
    uchar *ip;
    socklen_t len;
    
    if(fdwait(fd, 'r') == -1) {
        return -1;
    }

    taskstate("netaccept");
    len = sizeof sa;
    if((cfd = accept(fd, (void*)&sa, &len)) < 0){
        taskstate("accept failed");
        return -1;
    }
    if(server){
        ip = (uchar*)&sa.sin_addr;
        snprint(server, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    }
    if(port)
        *port = ntohs(sa.sin_port);
    fdnoblock(cfd);
    one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);
    taskstate("netaccept succeeded");
    return cfd;
}

#define CLASS(p) ((*(unsigned char*)(p))>>6)
static int
parseip(char *name, uint32_t *ip)
{
    union {
        char bytes[4];
        uint32_t intval;
    } addr;
    char *p;
    int i, x;

    p = name;
    for(i=0; i<4 && *p; i++){
        x = strtoul(p, &p, 0);
        if(x < 0 || x >= 256)
            return -1;
        if(*p != '.' && *p != 0)
            return -1;
        if(*p == '.')
            p++;
        addr.bytes[i] = x;
    }

    switch(CLASS(addr.bytes)){
    case 0:
    case 1:
        if(i == 3){
            addr.bytes[3] = addr.bytes[2];
            addr.bytes[2] = addr.bytes[1];
            addr.bytes[1] = 0;
        }else if(i == 2){
            addr.bytes[3] = addr.bytes[1];
            addr.bytes[2] = 0;
            addr.bytes[1] = 0;
        }else if(i != 4)
            return -1;
        break;
    case 2:
        if(i == 3){
            addr.bytes[3] = addr.bytes[2];
            addr.bytes[2] = 0;
        }else if(i != 4)
            return -1;
        break;
    }
    *ip = addr.intval;
    return 0;
}

int
netlookup(char *name, uint32_t *ip)
{
    struct hostent *he;

    if(parseip(name, ip) >= 0)
        return 0;
    
    /* BUG - Name resolution blocks.  Need a non-blocking DNS. */
    taskstate("netlookup");
    if((he = gethostbyname(name)) != 0){
        *ip = *(uint32_t*)he->h_addr;
        taskstate("netlookup succeeded");
        return 0;
    }
    
    taskstate("netlookup failed");
    return -1;
}

int
netdial(int istcp, char *server, int port)
{
    int proto, fd, n;
    uint32_t ip;
    struct sockaddr_in sa;
    socklen_t sn;
    
    if(netlookup(server, &ip) < 0)
        return -1;

    taskstate("netdial");
    proto = istcp ? SOCK_STREAM : SOCK_DGRAM;
    if((fd = socket(AF_INET, proto, 0)) < 0){
        taskstate("socket failed");
        return -1;
    }
    fdnoblock(fd);

    /* for udp */
    if(!istcp){
        n = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &n, sizeof n);
    }
    
    /* start connecting */
    memset(&sa, 0, sizeof sa);
    memmove(&sa.sin_addr, &ip, 4);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if(connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0 && errno != EINPROGRESS){
        taskstate("connect failed");
        fdclose(fd);
        return -1;
    }

    /* wait for finish */    
    if(fdwait(fd, 'w') == -1) return -1;

    sn = sizeof sa;
    if(getpeername(fd, (struct sockaddr*)&sa, &sn) >= 0){
        taskstate("connect succeeded");
        return fd;
    }
    
    /* report error */
    sn = sizeof n;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&n, &sn);
    if(n == 0)
        n = ECONNREFUSED;
    fdclose(fd);
    taskstate("connect failed");
    errno = n;
    return -1;
}

