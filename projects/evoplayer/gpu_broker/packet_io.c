/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "packet_io.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int ps5_gpu_packet_socketpair(int pair[2], size_t packet_size)
{
    if(packet_size>65536) { errno=E2BIG;return -1; }
    if(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair))return -1;
    int bytes=(int)packet_size*4;
    for(unsigned i=0;i<2;++i)
        if(setsockopt(pair[i],SOL_SOCKET,SO_SNDBUF,&bytes,sizeof(bytes)) ||
           setsockopt(pair[i],SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes))) {
            int saved=errno;close(pair[0]);close(pair[1]);pair[0]=pair[1]=-1;errno=saved;return -1;
        }
    return 0;
}

int ps5_gpu_packet_send(int fd, const void *data, size_t size, const int *fds, unsigned count)
{
    struct iovec vec={(void *)data,size};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(2*sizeof(int))]; } control={0};
    struct msghdr msg={0};
    if(count>2) { errno=EINVAL;return -1; }
    msg.msg_iov=&vec;msg.msg_iovlen=1;
    if(count) {
        msg.msg_control=control.bytes;msg.msg_controllen=CMSG_SPACE(count*sizeof(int));
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
        c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(CMSG_DATA(c),fds,count*sizeof(int));
    }
    ssize_t sent;
    do sent=sendmsg(fd,&msg,MSG_NOSIGNAL);while(sent<0 && errno==EINTR);
    if(sent==(ssize_t)size)return 0;
    if(sent>=0)errno=EIO;
    return -1;
}
ssize_t ps5_gpu_packet_recv(int fd, void *data, size_t size, int fds[2], unsigned *count)
{
    struct iovec vec={data,size};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(2*sizeof(int))]; } control={0};
    struct msghdr msg={0};
    msg.msg_iov=&vec;msg.msg_iovlen=1;msg.msg_control=control.bytes;msg.msg_controllen=sizeof(control.bytes);
    *count=0;fds[0]=fds[1]=-1;
    ssize_t received;
    do received=recvmsg(fd,&msg,0);while(received<0 && errno==EINTR);
    if(received<0)return received;
    int invalid=msg.msg_flags & (MSG_TRUNC|MSG_CTRUNC);
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0)) { invalid=1;continue; }
        size_t bytes=c->cmsg_len-CMSG_LEN(0);
        if(bytes%sizeof(int))invalid=1;
        for(size_t i=0;i<bytes/sizeof(int);++i) {
            int passed;memcpy(&passed,(char *)CMSG_DATA(c)+i*sizeof(int),sizeof(passed));
            if(*count<2)fds[(*count)++]=passed;
            else { close(passed);invalid=1; }
        }
    }
    if(invalid) {
        for(unsigned i=0;i<*count;++i)close(fds[i]);
        *count=0;fds[0]=fds[1]=-1;errno=EMSGSIZE;return -1;
    }
    return received;
}
