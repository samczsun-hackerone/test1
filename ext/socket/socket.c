/************************************************

  socket.c -

  $Author$
  $Date$
  created at: Thu Mar 31 12:21:29 JST 1994

************************************************/

#include "ruby.h"
#include "rubyio.h"
#include "rubysig.h"
#include <stdio.h>
#include <sys/types.h>
#ifndef NT
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#ifdef USE_CWGUSI
extern int fileno(FILE *stream); /* <unix.mac.h> */
extern int rb_thread_select(int, fd_set*, fd_set*, fd_set*, struct timeval*); /* thread.c */
# include <sys/errno.h>
# include <GUSI.h>
#endif

#if defined(HAVE_FCNTL)
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef HAVE_GETADDRINFO
# include "addrinfo.h"
#endif
#include "sockport.h"

static int do_not_reverse_lookup = 0;

VALUE rb_cBasicSocket;
VALUE rb_cIPSocket;
VALUE rb_cTCPSocket;
VALUE rb_cTCPServer;
VALUE rb_cUDPSocket;
#ifdef AF_UNIX
VALUE rb_cUNIXSocket;
VALUE rb_cUNIXServer;
#endif
VALUE rb_cSocket;

static VALUE rb_eSocket;

#ifdef SOCKS
VALUE rb_cSOCKSSocket;
void SOCKSinit();
int Rconnect();
#endif

#define INET_CLIENT 0
#define INET_SERVER 1
#define INET_SOCKS  2

#ifndef INET6
# undef  ss_family
# define sockaddr_storage	sockaddr
# define ss_family		sa_family
#endif

#ifdef NT
static void
sock_finalize(fptr)
    OpenFile *fptr;
{
    SOCKET s;
	extern int errno;

    if (!fptr->f) return;

	myfdclose(fptr->f);
	if(fptr->f2)  myfdclose(fptr->f);
/*
	s = get_osfhandle(fileno(fptr->f));
    closesocket(s);
*/
}
#endif

static VALUE
sock_new(class, fd)
    VALUE class;
    int fd;
{
    OpenFile *fp;
    NEWOBJ(sock, struct RFile);
    OBJSETUP(sock, class, T_FILE);

    rb_secure(4);
    MakeOpenFile(sock, fp);
    fp->f = rb_fdopen(fd, "r");
#ifdef NT
    fp->finalize = sock_finalize;
#else
    fd = dup(fd);
#endif
    fp->f2 = rb_fdopen(fd, "w");
    fp->mode = FMODE_READWRITE;
    rb_io_unbuffered(fp);

    return (VALUE)sock;
}

static VALUE
bsock_shutdown(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    VALUE howto;
    int how;
    OpenFile *fptr;

    rb_secure(4);
    rb_scan_args(argc, argv, "01", &howto);
    if (howto == Qnil)
	how = 2;
    else {
	how = NUM2INT(howto);
	if (how < 0 || 2 < how) {
	    rb_raise(rb_eArgError, "`how' should be either 0, 1, 2");
	}
    }
    GetOpenFile(sock, fptr);
    if (shutdown(fileno(fptr->f), how) == -1)
	rb_sys_fail(0);

    return INT2FIX(0);
}

static VALUE
bsock_close_read(sock)
    VALUE sock;
{
    OpenFile *fptr;

    rb_secure(4);
    GetOpenFile(sock, fptr);
    shutdown(fileno(fptr->f), 0);
    if (fptr->f2 == 0) {
	return rb_io_close(sock);
    }
    rb_thread_fd_close(fileno(fptr->f));
    fptr->mode &= ~FMODE_READABLE;
#ifdef NT
    free(fptr->f);
#else
    fclose(fptr->f);
#endif
    fptr->f = fptr->f2;
    fptr->f2 = 0;

    return Qnil;
}

static VALUE
bsock_close_write(sock)
    VALUE sock;
{
    OpenFile *fptr;

    rb_secure(4);
    GetOpenFile(sock, fptr);
    if (fptr->f2 == 0) {
	return rb_io_close(sock);
    }
    shutdown(fileno(fptr->f2), 1);
    fptr->mode &= ~FMODE_WRITABLE;
#ifdef NT
    free(fptr->f2);
#else
    fclose(fptr->f2);
#endif
    fptr->f2 = 0;

    return Qnil;
}

static VALUE
bsock_setsockopt(sock, lev, optname, val)
    VALUE sock, lev, optname, val;
{
    int level, option;
    OpenFile *fptr;
    int i;
    char *v;
    int vlen;

    rb_secure(2);
    level = NUM2INT(lev);
    option = NUM2INT(optname);
    switch (TYPE(val)) {
      case T_FIXNUM:
	i = FIX2INT(val);
	goto numval;
      case T_FALSE:
	i = 0;
	goto numval;
      case T_TRUE:
	i = 1;
      numval:
	v = (char*)&i; vlen = sizeof(i);
	break;
      default:
	v = rb_str2cstr(val, &vlen);
    }

    GetOpenFile(sock, fptr);
    if (setsockopt(fileno(fptr->f), level, option, v, vlen) < 0)
	rb_sys_fail(fptr->path);

    return INT2FIX(0);
}

static VALUE
bsock_getsockopt(sock, lev, optname)
    VALUE sock, lev, optname;
{
#if !defined(__BEOS__)
    int level, option, len;
    char *buf;
    OpenFile *fptr;

    level = NUM2INT(lev);
    option = NUM2INT(optname);
    len = 256;
    buf = ALLOCA_N(char,len);

    GetOpenFile(sock, fptr);
    if (getsockopt(fileno(fptr->f), level, option, buf, &len) < 0)
	rb_sys_fail(fptr->path);

    return rb_str_new(buf, len);
#else
    rb_notimplement();
#endif
}

static VALUE
bsock_getsockname(sock)
   VALUE sock;
{
    char buf[1024];
    int len = sizeof buf;
    OpenFile *fptr;

    GetOpenFile(sock, fptr);
    if (getsockname(fileno(fptr->f), (struct sockaddr*)buf, &len) < 0)
	rb_sys_fail("getsockname(2)");
    return rb_str_new(buf, len);
}

static VALUE
bsock_getpeername(sock)
   VALUE sock;
{
    char buf[1024];
    int len = sizeof buf;
    OpenFile *fptr;

    GetOpenFile(sock, fptr);
    if (getpeername(fileno(fptr->f), (struct sockaddr*)buf, &len) < 0)
	rb_sys_fail("getpeername(2)");
    return rb_str_new(buf, len);
}

static VALUE
bsock_send(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    VALUE msg, to;
    VALUE flags;
    OpenFile *fptr;
    FILE *f;
    int fd, n;
    char *m, *t;
    int mlen, tlen;

    rb_secure(4);
    rb_scan_args(argc, argv, "21", &msg, &flags, &to);

    GetOpenFile(sock, fptr);
    f = GetWriteFile(fptr);
    fd = fileno(f);
  retry:
    rb_thread_fd_writable(fd);
    m = rb_str2cstr(msg, &mlen);
    if (RTEST(to)) {
	t = rb_str2cstr(to, &tlen);
	n = sendto(fd, m, mlen, NUM2INT(flags),
		   (struct sockaddr*)t, tlen);
    }
    else {
	n = send(fd, m, mlen, NUM2INT(flags));
    }
    if (n < 0) {
	switch (errno) {
	  case EINTR:
	    rb_thread_schedule();
	    goto retry;
	  case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	  case EAGAIN:
#endif
	    rb_thread_fd_writable(fd);
	    goto retry;
	}
	rb_sys_fail("send(2)");
    }
    return INT2FIX(n);
}

static VALUE ipaddr _((struct sockaddr *));
#ifdef HAVE_SYS_UN_H
static VALUE unixaddr _((struct sockaddr_un *));
#endif

enum sock_recv_type {
    RECV_RECV,			/* BasicSocket#recv(no from) */
    RECV_TCP,			/* TCPSocket#recvfrom */
    RECV_UDP,			/* UDPSocket#recvfrom */
    RECV_UNIX,			/* UNIXSocket#recvfrom */
    RECV_SOCKET,		/* Socket#recvfrom */
};

static VALUE
s_recv(sock, argc, argv, from)
    VALUE sock;
    int argc;
    VALUE *argv;
    enum sock_recv_type from;
{
    OpenFile *fptr;
    VALUE str;
    char buf[1024];
    int fd, alen = sizeof buf;
    VALUE len, flg;
    int flags;

    rb_scan_args(argc, argv, "11", &len, &flg);

    if (flg == Qnil) flags = 0;
    else             flags = NUM2INT(flg);

    str = rb_str_new(0, NUM2INT(len));

    GetOpenFile(sock, fptr);
    fd = fileno(fptr->f);
    rb_thread_wait_fd(fd);
    TRAP_BEG;
  retry:
    RSTRING(str)->len = recvfrom(fd, RSTRING(str)->ptr, RSTRING(str)->len, flags,
				 (struct sockaddr*)buf, &alen);
    TRAP_END;

    if (RSTRING(str)->len < 0) {
	switch (errno) {
	  case EINTR:
	    rb_thread_schedule();
	    goto retry;

	  case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	  case EAGAIN:
#endif
	    rb_thread_wait_fd(fd);
	    goto retry;
	}
	rb_sys_fail("recvfrom(2)");
    }
    rb_obj_taint(str);
    switch (from) {
      case RECV_RECV:
	return (VALUE)str;
      case RECV_TCP:
      case RECV_UDP:
#if 0
	if (alen != sizeof(struct sockaddr_in)) {
	    rb_raise(rb_eTypeError, "sockaddr size differs - should not happen");
	}
#endif
	return rb_assoc_new(str, ipaddr((struct sockaddr *)buf));
#ifdef HAVE_SYS_UN_H
      case RECV_UNIX:
	return rb_assoc_new(str, unixaddr((struct sockaddr_un *)buf));
#endif
      case RECV_SOCKET:
	return rb_assoc_new(str, rb_str_new(buf, alen));
    }
}

static VALUE
bsock_recv(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    return s_recv(sock, argc, argv, RECV_RECV);
}

static VALUE
bsock_do_not_rev_lookup()
{
    return do_not_reverse_lookup?Qtrue:Qfalse;
}

static VALUE
bsock_do_not_rev_lookup_set(self, val)
{
    do_not_reverse_lookup = RTEST(val);
    return val;
}

static void
mkipaddr0(addr, buf, len)
    struct sockaddr *addr;
    char *buf;
    size_t len;
{
    int error;

    error = getnameinfo(addr, SA_LEN(addr), buf, len, NULL, 0,
			NI_NUMERICHOST);
    if (error) {
	rb_raise(rb_eSocket, "%s", gai_strerror(error));
    }
}

static VALUE
mkipaddr(addr)
    struct sockaddr *addr;
{
    char buf[1024];

    mkipaddr0(addr, buf, sizeof(buf));
    return rb_str_new2(buf);
}

static void
mkinetaddr(host, buf, len)
    long host;
    char *buf;
    size_t len;
{
    struct sockaddr_in sin;

    MEMZERO(&sin, struct sockaddr_in, 1);
    sin.sin_family = AF_INET;
    SET_SIN_LEN(&sin, sizeof(sin));
    sin.sin_addr.s_addr = host;
    mkipaddr0((struct sockaddr *)&sin, buf, len);
}

static struct addrinfo*
ip_addrsetup(host, port)
    VALUE host, port;
{
    struct addrinfo hints, *res;
    char *hostp, *portp;
    int error;
    char hbuf[1024], pbuf[16];

    if (NIL_P(host)) {
	hostp = NULL;
    }
    else if (rb_obj_is_kind_of(host, rb_cInteger)) {
	long i = NUM2LONG(host);

	mkinetaddr(htonl(i), hbuf, sizeof(hbuf));
    }
    else {
	char *name = STR2CSTR(host);

	if (*name == 0) {
	    mkinetaddr(INADDR_ANY, hbuf, sizeof(hbuf));
	}
	else if (name[0] == '<' && strcmp(name, "<broadcast>") == 0) {
	    mkinetaddr(INADDR_BROADCAST, hbuf, sizeof(hbuf));
	}
	else {
	    strcpy(hbuf, name);
	}
    }
    hostp = hbuf;
    if (NIL_P(port)) {
	portp = 0;
    }
    else if (FIXNUM_P(port)) {
	snprintf(pbuf, sizeof(pbuf), "%d", FIX2INT(port));
	portp = pbuf;
    }
    else {
	portp = STR2CSTR(port);
    }

    MEMZERO(&hints, struct addrinfo, 1);
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    error = getaddrinfo(hostp, portp, &hints, &res);
    if (error) {
	rb_raise(rb_eSocket, "%s", gai_strerror(error));
    }

    return res;
}

static void
setipaddr(name, addr)
    VALUE name;
    struct sockaddr *addr;
{
    struct addrinfo *res = ip_addrsetup(name, Qnil);

    /* just take the first one */
    memcpy(addr, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
}

static VALUE
ipaddr(sockaddr)
    struct sockaddr *sockaddr;
{
    VALUE family, port, addr1, addr2;
    VALUE ary;
    int error;
    char hbuf[1024], pbuf[1024];

    switch (sockaddr->sa_family) {
    case AF_INET:
	family = rb_str_new2("AF_INET");
	break;
#ifdef INET6
    case AF_INET6:
	family = rb_str_new2("AF_INET6");
	break;
#endif
    default:
	family = 0;
	break;
    }
    if (!do_not_reverse_lookup) {
	error = getnameinfo(sockaddr, SA_LEN(sockaddr), hbuf, sizeof(hbuf),
			    NULL, 0, 0);
	if (error) {
	    rb_raise(rb_eSocket, "%s", gai_strerror(error));
	}
	addr1 = rb_str_new2(hbuf);
    }
    error = getnameinfo(sockaddr, SA_LEN(sockaddr), hbuf, sizeof(hbuf),
			pbuf, sizeof(pbuf), NI_NUMERICHOST | NI_NUMERICSERV);
    if (error) {
	rb_raise(rb_eSocket, "%s", gai_strerror(error));
    }
    addr2 = rb_str_new2(hbuf);
    if (do_not_reverse_lookup) {
	addr1 = addr2;
    }
    port = INT2FIX(atoi(pbuf));
    ary = rb_ary_new3(4, family, port, addr1, addr2);

    return ary;
}

static void
thread_write_select(fd)
    int fd;
{
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    rb_thread_select(fd+1, 0, &fds, 0, 0);
}

static int
ruby_socket(domain, type, proto)
    int domain, type, proto;
{
    int fd;

    fd = socket(domain, type, proto);
    if (fd < 0) {
	if (errno == EMFILE || errno == ENFILE) {
	    rb_gc();
	    fd = socket(domain, type, proto);
	}
    }
    return fd;
}

static int
ruby_connect(fd, sockaddr, len, socks)
    int fd;
    struct sockaddr *sockaddr;
    int len;
    int socks;
{
    int status;
    int mode;

#if defined(HAVE_FCNTL)
    mode = fcntl(fd, F_GETFL, 0);

#ifdef O_NDELAY 
# define NONBLOCKING O_NDELAY
#else
#ifdef O_NBIO
# define NONBLOCKING O_NBIO
#else
# define NONBLOCKING O_NONBLOCK
#endif
#endif
    fcntl(fd, F_SETFL, mode|NONBLOCKING);
#endif /* HAVE_FCNTL */

    for (;;) {
#ifdef SOCKS
	if (socks) {
	    status = Rconnect(fd, sockaddr, len);
	}
	else
#endif
	{
	    status = connect(fd, sockaddr, len);
	}
	if (status < 0) {
	    switch (errno) {
	      case EAGAIN:
#ifdef EINPROGRESS
	      case EINPROGRESS:
#endif
		thread_write_select(fd);
		continue;

#ifdef EISCONN
	      case EISCONN:
		status = 0;
		errno = 0;
		break;
#endif
	    }
	}
#ifdef HAVE_FCNTL
	mode &= ~NONBLOCKING;
	fcntl(fd, F_SETFL, mode);
#endif
	return status;
    }
}

static VALUE
open_inet(class, h, serv, type)
    VALUE class, h, serv;
    int type;
{
    struct addrinfo hints, *res, *res0;
    int fd, status;
    char *syscall;
    char pbuf[1024], *portp;
    char *host;
    int error;

    if (h) {
	Check_SafeStr(h);
	host = RSTRING(h)->ptr;
    }
    else {
	host = NULL;
    }
    if (FIXNUM_P(serv)) {
	snprintf(pbuf, sizeof(pbuf), "%d", FIX2UINT(serv));
	portp = pbuf;
    }
    else {
	strcpy(pbuf, STR2CSTR(serv));
	portp = pbuf;
    }
    MEMZERO(&hints, struct addrinfo, 1);
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (type == INET_SERVER) {
	hints.ai_flags = AI_PASSIVE;
    }
    error = getaddrinfo(host, portp, &hints, &res0);
    if (error) {
	rb_raise(rb_eSocket, "%s", gai_strerror(error));
    }

    fd = -1;
    for (res = res0; res; res = res->ai_next) {
	status = ruby_socket(res->ai_family,res->ai_socktype,res->ai_protocol);
	syscall = "socket(2)";
	fd = status;
	if (fd < 0) {
	    continue;
	}
	if (type == INET_SERVER) {
	    status = 1;
	    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		       (char*)&status, sizeof(status));
	    status = bind(fd, res->ai_addr, res->ai_addrlen);
	    syscall = "bind(2)";
	}
	else {
	    status = ruby_connect(fd, res->ai_addr, res->ai_addrlen,
				  (type == INET_SOCKS));
	    syscall = "connect(2)";
	}

	if (status < 0) {
	    close(fd);
	    fd = -1;
	    continue;
	} else
	    break;
    }
    if (status < 0) {
	if (fd >= 0)
	    close(fd);
	freeaddrinfo(res0);
	rb_sys_fail(syscall);
    }

    if (type == INET_SERVER)
	listen(fd, 5);

    /* create new instance */
    freeaddrinfo(res0);
    return sock_new(class, fd);
}

static VALUE
tcp_s_open(class, host, serv)
    VALUE class, host, serv;
{
    Check_SafeStr(host);
    return open_inet(class, host, serv, INET_CLIENT);
}

#ifdef SOCKS
static VALUE
socks_s_open(class, host, serv)
    VALUE class, host, serv;
{
    static init = 0;

    if (init == 0) {
	SOCKSinit("ruby");
	init = 1;
    }
	
    Check_SafeStr(host);
    return open_inet(class, host, serv, INET_SOCKS);
}
#endif

/*
 * NOTE: using gethostbyname() against AF_INET6 is a bad idea, as it
 * does not initialize sin_flowinfo nor sin_scope_id properly.
 */
static VALUE
tcp_s_gethostbyname(obj, host)
    VALUE obj, host;
{
    struct sockaddr_storage addr;
    struct hostent *h;
    char **pch;
    VALUE ary, names;

    if (rb_obj_is_kind_of(host, rb_cInteger)) {
	long i = NUM2LONG(host);
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&addr;
	MEMZERO(sin, struct sockaddr_in, 1);
	sin->sin_family = AF_INET;
	SET_SIN_LEN(sin, sizeof(*sin));
	sin->sin_addr.s_addr = htonl(i);
    }
    else {
	setipaddr(host, &addr);
    }
    switch (addr.ss_family) {
    case AF_INET:
      {
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&addr;
	h = gethostbyaddr((char *)&sin->sin_addr,
			  sizeof(sin->sin_addr),
			  sin->sin_family);
	break;
      }
#ifdef INET6
    case AF_INET6:
      {
	struct sockaddr_in6 *sin6;
	sin6 = (struct sockaddr_in6 *)&addr;
	h = gethostbyaddr((char *)&sin6->sin6_addr,
			  sizeof(sin6->sin6_addr),
			  sin6->sin6_family);
	break;
      }
#endif
    default:
	h = NULL;
    }

    if (h == NULL) {
#ifdef HAVE_HSTERROR
	extern int h_errno;
	rb_raise(rb_eSocket, "%s", (char *)hsterror(h_errno));
#else
	rb_raise(rb_eSocket, "host not found");
#endif
    }
    ary = rb_ary_new();
    rb_ary_push(ary, rb_str_new2(h->h_name));
    names = rb_ary_new();
    rb_ary_push(ary, names);
    for (pch = h->h_aliases; *pch; pch++) {
	rb_ary_push(names, rb_str_new2(*pch));
    }
    rb_ary_push(ary, INT2NUM(h->h_addrtype));
#ifdef h_addr
    for (pch = h->h_addr_list; *pch; pch++) {
	switch (addr.ss_family) {
	case AF_INET:
	  {
	    struct sockaddr_in sin;
	    MEMZERO(&sin, struct sockaddr_in, 1);
	    sin.sin_family = AF_INET;
	    SET_SIN_LEN(&sin, sizeof(sin));
	    memcpy((char *) &sin.sin_addr, *pch, h->h_length);
	    h = gethostbyaddr((char *)&sin.sin_addr,
			      sizeof(sin.sin_addr),
			      sin.sin_family);
	    rb_ary_push(ary, mkipaddr((struct sockaddr *)&sin));
	    break;
	  }
#ifdef INET6
	case AF_INET6:
	  {
	    struct sockaddr_in6 sin6;
	    MEMZERO(&sin6, struct sockaddr_in6, 1);
	    sin6.sin6_family = AF_INET;
	    sin6.sin6_len = sizeof(sin6);
	    memcpy((char *) &sin6.sin6_addr, *pch, h->h_length);
	    h = gethostbyaddr((char *)&sin6.sin6_addr,
			      sizeof(sin6.sin6_addr),
			      sin6.sin6_family);
	    rb_ary_push(ary, mkipaddr((struct sockaddr *)&sin6));
	    break;
	  }
#endif
	default:
	    h = NULL;
	}
    }
#else
    memcpy((char *)&addr.sin_addr, h->h_addr, h->h_length);
    rb_ary_push(ary, mkipaddr((struct sockaddr *)&addr));
#endif

    return ary;
}

static VALUE
tcp_svr_s_open(argc, argv, class)
    int argc;
    VALUE *argv;
    VALUE class;
{
    VALUE arg1, arg2;

    if (rb_scan_args(argc, argv, "11", &arg1, &arg2) == 2)
	return open_inet(class, arg1, arg2, INET_SERVER);
    else
	return open_inet(class, 0, arg1, INET_SERVER);
}

static VALUE
s_accept(class, fd, sockaddr, len)
    VALUE class;
    int fd;
    struct sockaddr *sockaddr;
    int *len;
{
    int fd2;

  retry:
    rb_thread_wait_fd(fd);
    TRAP_BEG;
    fd2 = accept(fd, sockaddr, len);
    TRAP_END;
    if (fd2 < 0) {
	switch (errno) {
	  case EINTR:
	    rb_thread_schedule();
	    goto retry;

	  case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	  case EAGAIN:
#endif
	    rb_thread_wait_fd(fd);
	    goto retry;
	}
	rb_sys_fail(0);
    }
    return sock_new(class, fd2);
}

static VALUE
tcp_accept(sock)
    VALUE sock;
{
    OpenFile *fptr;
    struct sockaddr_storage from;
    int fromlen;

    GetOpenFile(sock, fptr);
    fromlen = sizeof(from);
    return s_accept(rb_cTCPSocket, fileno(fptr->f),
		    (struct sockaddr*)&from, &fromlen);
}

static VALUE
tcp_recvfrom(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    return s_recv(sock, argc, argv, RECV_TCP);
}

#ifdef HAVE_SYS_UN_H
static VALUE
open_unix(class, path, server)
    VALUE class;
    struct RString *path;
    int server;
{
    struct sockaddr_un sockaddr;
    int fd, status;
    VALUE sock;
    OpenFile *fptr;

    Check_SafeStr(path);
    fd = ruby_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
	rb_sys_fail("socket(2)");
    }

    MEMZERO(&sockaddr, struct sockaddr_un, 1);
    sockaddr.sun_family = AF_UNIX;
    strncpy(sockaddr.sun_path, path->ptr, sizeof(sockaddr.sun_path)-1);
    sockaddr.sun_path[sizeof(sockaddr.sun_path)-1] = '\0';

    if (server) {
        status = bind(fd, (struct sockaddr*)&sockaddr, sizeof(sockaddr));
    }
    else {
        status = ruby_connect(fd, (struct sockaddr*)&sockaddr, sizeof(sockaddr), 0);
    }

    if (status < 0) {
	close(fd);
	rb_sys_fail(sockaddr.sun_path);
    }

    if (server) listen(fd, 5);

    sock = sock_new(class, fd);
    GetOpenFile(sock, fptr);
    fptr->path = strdup(path->ptr);

    return sock;
}
#endif

static VALUE
ip_addr(sock)
    VALUE sock;
{
    OpenFile *fptr;
    struct sockaddr_storage addr;
    int len = sizeof addr;

    GetOpenFile(sock, fptr);

    if (getsockname(fileno(fptr->f), (struct sockaddr*)&addr, &len) < 0)
	rb_sys_fail("getsockname(2)");
    return ipaddr((struct sockaddr *)&addr);
}

static VALUE
ip_peeraddr(sock)
    VALUE sock;
{
    OpenFile *fptr;
    struct sockaddr_storage addr;
    int len = sizeof addr;

    GetOpenFile(sock, fptr);

    if (getpeername(fileno(fptr->f), (struct sockaddr*)&addr, &len) < 0)
	rb_sys_fail("getpeername(2)");
    return ipaddr((struct sockaddr *)&addr);
}

static VALUE
ip_s_getaddress(obj, host)
    VALUE obj, host;
{
    struct sockaddr_storage addr;

    setipaddr(host, &addr);
    return mkipaddr((struct sockaddr *)&addr);
}

static VALUE
udp_s_open(argc, argv, class)
    int argc;
    VALUE *argv;
    VALUE class;
{
    VALUE arg;
    int socktype = AF_INET;
    int fd;

    if (rb_scan_args(argc, argv, "01", &arg) == 1) {
	socktype = NUM2INT(arg);
    }
    fd = ruby_socket(socktype, SOCK_DGRAM, 0);
    if (fd < 0) {
	rb_sys_fail("socket(2) - udp");
    }

    return sock_new(class, fd);
}

static VALUE
udp_connect(sock, host, port)
    VALUE sock, host, port;
{
    OpenFile *fptr;
    int fd;
    struct addrinfo *res0, *res;

    GetOpenFile(sock, fptr);
    fd = fileno(fptr->f);
    res0 = ip_addrsetup(host, port);
    for (res = res0; res; res = res->ai_next) {
	if (ruby_connect(fd, res->ai_addr, res->ai_addrlen, 0) >= 0) {
	    freeaddrinfo(res0);
	    return INT2FIX(0);
	}
    }

    freeaddrinfo(res0);
    rb_sys_fail("connect(2)");
    return INT2FIX(0);
}

static VALUE
udp_bind(sock, host, port)
    VALUE sock, host, port;
{
    OpenFile *fptr;
    struct addrinfo *res0, *res;

    GetOpenFile(sock, fptr);
    res0 = ip_addrsetup(host, port);
    for (res = res0; res; res = res->ai_next) {
	if (bind(fileno(fptr->f), res->ai_addr, res->ai_addrlen) < 0) {
	    continue;
	}
	freeaddrinfo(res0);
	return INT2FIX(0);
    }
    freeaddrinfo(res0);
    rb_sys_fail("bind(2)");
    return INT2FIX(0);
}

static VALUE
udp_send(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    VALUE mesg, flags, host, port;
    OpenFile *fptr;
    FILE *f;
    int n;
    char *m;
    int mlen;
    struct addrinfo *res0, *res;

    if (argc == 2) {
	return bsock_send(argc, argv, sock);
    }
    rb_scan_args(argc, argv, "4", &mesg, &flags, &host, &port);

    GetOpenFile(sock, fptr);
    res0 = ip_addrsetup(host, port);
    f = GetWriteFile(fptr);
    m = rb_str2cstr(mesg, &mlen);
    for (res = res0; res; res = res->ai_next) {
  retry:
	n = sendto(fileno(f), m, mlen, NUM2INT(flags), res->ai_addr,
		    res->ai_addrlen);
	if (n >= 0) {
	    freeaddrinfo(res0);
	    return INT2FIX(n);
	}
	switch (errno) {
	  case EINTR:
	    rb_thread_schedule();
	    goto retry;

	  case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
	  case EAGAIN:
#endif
	    thread_write_select(fileno(f));
	    goto retry;
	}
    }
    freeaddrinfo(res0);
    rb_sys_fail("sendto(2)");
    return INT2FIX(n);
}

static VALUE
udp_recvfrom(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    return s_recv(sock, argc, argv, RECV_UDP);
}

#ifdef HAVE_SYS_UN_H
static VALUE
unix_s_sock_open(sock, path)
    VALUE sock, path;
{
    return open_unix(sock, path, 0);
}

static VALUE
unix_path(sock)
    VALUE sock;
{
    OpenFile *fptr;

    GetOpenFile(sock, fptr);
    if (fptr->path == 0) {
	struct sockaddr_un addr;
	int len = sizeof(addr);
	if (getsockname(fileno(fptr->f), (struct sockaddr*)&addr, &len) < 0)
	    rb_sys_fail(0);
	fptr->path = strdup(addr.sun_path);
    }
    return rb_str_new2(fptr->path);
}

static VALUE
unix_svr_s_open(sock, path)
    VALUE sock, path;
{
    return open_unix(sock, path, 1);
}

static VALUE
unix_recvfrom(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    return s_recv(sock, argc, argv, RECV_UNIX);
}

static VALUE
unix_accept(sock)
    VALUE sock;
{
    OpenFile *fptr;
    struct sockaddr_un from;
    int fromlen;

    GetOpenFile(sock, fptr);
    fromlen = sizeof(struct sockaddr_un);
    return s_accept(rb_cUNIXSocket, fileno(fptr->f),
		    (struct sockaddr*)&from, &fromlen);
}

static VALUE
unixaddr(sockaddr)
    struct sockaddr_un *sockaddr;
{
    return rb_assoc_new(rb_str_new2("AF_UNIX"),rb_str_new2(sockaddr->sun_path));
}

static VALUE
unix_addr(sock)
    VALUE sock;
{
    OpenFile *fptr;
    struct sockaddr_un addr;
    int len = sizeof addr;

    GetOpenFile(sock, fptr);

    if (getsockname(fileno(fptr->f), (struct sockaddr*)&addr, &len) < 0)
	rb_sys_fail("getsockname(2)");
    return unixaddr(&addr);
}

static VALUE
unix_peeraddr(sock)
    VALUE sock;
{
    OpenFile *fptr;
    struct sockaddr_un addr;
    int len = sizeof addr;

    GetOpenFile(sock, fptr);

    if (getpeername(fileno(fptr->f), (struct sockaddr*)&addr, &len) < 0)
	rb_sys_fail("getsockname(2)");
    return unixaddr(&addr);
}
#endif

static void
setup_domain_and_type(domain, dv, type, tv)
    VALUE domain, type;
    int *dv, *tv;
{
    char *ptr;

    if (TYPE(domain) == T_STRING) {
	ptr = RSTRING(domain)->ptr;
	if (strcmp(ptr, "AF_INET") == 0)
	    *dv = AF_INET;
#ifdef AF_UNIX
	else if (strcmp(ptr, "AF_UNIX") == 0)
	    *dv = AF_UNIX;
#endif
#ifdef AF_ISO
	else if (strcmp(ptr, "AF_ISO") == 0)
	    *dv = AF_ISO;
#endif
#ifdef AF_NS
	else if (strcmp(ptr, "AF_NS") == 0)
	    *dv = AF_NS;
#endif
#ifdef AF_IMPLINK
	else if (strcmp(ptr, "AF_IMPLINK") == 0)
	    *dv = AF_IMPLINK;
#endif
#ifdef PF_INET
	else if (strcmp(ptr, "PF_INET") == 0)
	    *dv = PF_INET;
#endif
#ifdef PF_UNIX
	else if (strcmp(ptr, "PF_UNIX") == 0)
	    *dv = PF_UNIX;
#endif
#ifdef PF_IMPLINK
	else if (strcmp(ptr, "PF_IMPLINK") == 0)
	    *dv = PF_IMPLINK;
	else if (strcmp(ptr, "AF_IMPLINK") == 0)
	    *dv = AF_IMPLINK;
#endif
#ifdef PF_AX25
	else if (strcmp(ptr, "PF_AX25") == 0)
	    *dv = PF_AX25;
#endif
#ifdef PF_IPX
	else if (strcmp(ptr, "PF_IPX") == 0)
	    *dv = PF_IPX;
#endif
	else
	    rb_raise(rb_eSocket, "Unknown socket domain %s", ptr);
    }
    else {
	*dv = NUM2INT(domain);
    }
    if (TYPE(type) == T_STRING) {
	ptr = RSTRING(type)->ptr;
	if (strcmp(ptr, "SOCK_STREAM") == 0)
	    *tv = SOCK_STREAM;
	else if (strcmp(ptr, "SOCK_DGRAM") == 0)
	    *tv = SOCK_DGRAM;
#ifdef SOCK_RAW
	else if (strcmp(ptr, "SOCK_RAW") == 0)
	    *tv = SOCK_RAW;
#endif
#ifdef SOCK_SEQPACKET
	else if (strcmp(ptr, "SOCK_SEQPACKET") == 0)
	    *tv = SOCK_SEQPACKET;
#endif
#ifdef SOCK_RDM
	else if (strcmp(ptr, "SOCK_RDM") == 0)
	    *tv = SOCK_RDM;
#endif
#ifdef SOCK_PACKET
	else if (strcmp(ptr, "SOCK_PACKET") == 0)
	    *tv = SOCK_PACKET;
#endif
	else
	    rb_raise(rb_eSocket, "Unknown socket type %s", ptr);
    }
    else {
	*tv = NUM2INT(type);
    }
}

static VALUE
sock_s_open(class, domain, type, protocol)
    VALUE class, domain, type, protocol;
{
    int fd;
    int d, t;

    setup_domain_and_type(domain, &d, type, &t);
    fd = ruby_socket(d, t, NUM2INT(protocol));
    if (fd < 0) rb_sys_fail("socket(2)");

    return sock_new(class, fd);
}

static VALUE
sock_s_for_fd(class, fd)
    VALUE class, fd;
{
    return sock_new(class, NUM2INT(fd));
}

static VALUE
sock_s_socketpair(class, domain, type, protocol)
    VALUE class, domain, type, protocol;
{
#if !defined(NT) && !defined(__BEOS__) && !defined(__EMX__)
    int d, t, sp[2];

    setup_domain_and_type(domain, &d, type, &t);
  again:
    if (socketpair(d, t, NUM2INT(protocol), sp) < 0) {
	if (errno == EMFILE || errno == ENFILE) {
	    rb_gc();
	    goto again;
	}
	rb_sys_fail("socketpair(2)");
    }

    return rb_assoc_new(sock_new(class, sp[0]), sock_new(class, sp[1]));
#else
    rb_notimplement();
#endif
}

static VALUE
sock_connect(sock, addr)
    VALUE sock, addr;
{
    OpenFile *fptr;
    int fd;

    Check_Type(addr, T_STRING);
    rb_str_modify(addr);

    GetOpenFile(sock, fptr);
    fd = fileno(fptr->f);
    if (ruby_connect(fd, (struct sockaddr*)RSTRING(addr)->ptr, RSTRING(addr)->len, 0) < 0) {
	rb_sys_fail("connect(2)");
    }

    return INT2FIX(0);
}

static VALUE
sock_bind(sock, addr)
    VALUE sock, addr;
{
    OpenFile *fptr;

    Check_Type(addr, T_STRING);
    rb_str_modify(addr);

    GetOpenFile(sock, fptr);
    if (bind(fileno(fptr->f), (struct sockaddr*)RSTRING(addr)->ptr, RSTRING(addr)->len) < 0)
	rb_sys_fail("bind(2)");

    return INT2FIX(0);
}

static VALUE
sock_listen(sock, log)
   VALUE sock, log;
{
    OpenFile *fptr;

    GetOpenFile(sock, fptr);
    if (listen(fileno(fptr->f), NUM2INT(log)) < 0)
	rb_sys_fail("listen(2)");

    return INT2FIX(0);
}

static VALUE
sock_recvfrom(argc, argv, sock)
    int argc;
    VALUE *argv;
    VALUE sock;
{
    return s_recv(sock, argc, argv, RECV_SOCKET);
}

static VALUE
sock_accept(sock)
   VALUE sock;
{
    OpenFile *fptr;
    VALUE sock2;
    char buf[1024];
    int len = sizeof buf;

    GetOpenFile(sock, fptr);
    sock2 = s_accept(rb_cSocket,fileno(fptr->f),(struct sockaddr*)buf,&len);

    return rb_assoc_new(sock2, rb_str_new(buf, len));
}

#ifdef HAVE_GETHOSTNAME
static VALUE
sock_gethostname(obj)
    VALUE obj;
{
    char buf[1024];

    if (gethostname(buf, (int)sizeof buf - 1) < 0)
	rb_sys_fail("gethostname");

    buf[sizeof buf - 1] = '\0';
    return rb_str_new2(buf);
}
#else
#ifdef HAVE_UNAME

#include <sys/utsname.h>

static VALUE
sock_gethostname(obj)
    VALUE obj;
{
  struct utsname un;

  uname(&un);
  return rb_str_new2(un.nodename);
}
#else
static VALUE
sock_gethostname(obj)
    VALUE obj;
{
    rb_notimplement();
}
#endif
#endif

static VALUE
mkhostent(h)
    struct hostent *h;
{
    char **pch;
    VALUE ary, names;

    if (h == NULL) {
#ifdef HAVE_HSTRERROR
	extern int h_errno;
	rb_raise(rb_eSocket, "%s", (char *)hstrerror(h_errno));
#else
	rb_raise(rb_eSocket, "host not found");
#endif
    }
    ary = rb_ary_new();
    rb_ary_push(ary, rb_str_new2(h->h_name));
    names = rb_ary_new();
    rb_ary_push(ary, names);
    for (pch = h->h_aliases; *pch; pch++) {
	rb_ary_push(names, rb_str_new2(*pch));
    }
    rb_ary_push(ary, INT2NUM(h->h_addrtype));
#ifdef h_addr
    for (pch = h->h_addr_list; *pch; pch++) {
	rb_ary_push(ary, rb_str_new(*pch, h->h_length));
    }
#else
    rb_ary_push(ary, rb_str_new(h->h_addr, h->h_length));
#endif

    return ary;
}

static VALUE
mkaddrinfo(res0)
    struct addrinfo *res0;
{
    VALUE base, ary;
    struct addrinfo *res;

    if (res0 == NULL) {
	rb_raise(rb_eSocket, "host not found");
    }
    base = rb_ary_new();
    for (res = res0; res; res = res->ai_next) {
	ary = ipaddr(res->ai_addr);
	rb_ary_push(ary, INT2FIX(res->ai_family));
	rb_ary_push(ary, INT2FIX(res->ai_socktype));
	rb_ary_push(ary, INT2FIX(res->ai_protocol));
	rb_ary_push(base, ary);
    }
    return base;
}

/*
 * NOTE: using gethostbyname() against AF_INET6 is a bad idea, as it
 * does not initialize sin_flowinfo nor sin_scope_id properly.
 */
static VALUE
sock_s_gethostbyname(obj, host)
    VALUE obj, host;
{
    struct sockaddr_storage addr;
    struct hostent *h;

    if (rb_obj_is_kind_of(host, rb_cInteger)) {
	long i = NUM2LONG(host);
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&addr;
	MEMZERO(sin, struct sockaddr_in, 1);
	sin->sin_family = AF_INET;
	SET_SIN_LEN(sin, sizeof(*sin));
	sin->sin_addr.s_addr = htonl(i);
    }
    else {
	setipaddr(host, (struct sockaddr *)&addr);
    }
    switch (addr.ss_family) {
    case AF_INET:
      {
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&addr;
	h = gethostbyaddr((char *)&sin->sin_addr,
			  sizeof(sin->sin_addr),
			  sin->sin_family);
	break;
      }
#ifdef INET6
    case AF_INET6:
      {
	struct sockaddr_in6 *sin6;
	sin6 = (struct sockaddr_in6 *)&addr;
	h = gethostbyaddr((char *)&sin6->sin6_addr,
			  sizeof(sin6->sin6_addr),
			  sin6->sin6_family);
	break;
      }
#endif
    default:
	h = NULL;
    }

    return mkhostent(h);
}

static VALUE
sock_s_gethostbyaddr(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE vaddr, vtype;
    int type;
    int alen;
    char *addr;
    struct hostent *h;

    rb_scan_args(argc, argv, "11", &vaddr, &vtype);
    addr = rb_str2cstr(vaddr, &alen);
    if (!NIL_P(vtype)) {
	type = NUM2INT(vtype);
    }
    else {
	type = AF_INET;
    }

    h = gethostbyaddr(addr, alen, type);

    return mkhostent(h);
}

static VALUE
sock_s_getservbyaname(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE service, protocol;
    char *proto;
    struct servent *sp;
    int port;

    rb_scan_args(argc, argv, "11", &service, &protocol);
    if (NIL_P(protocol)) proto = "tcp";
    else proto = STR2CSTR(protocol);

    sp = getservbyname(STR2CSTR(service), proto);
    if (sp) {
	port = ntohs(sp->s_port);
    }
    else {
	char *s = STR2CSTR(service);
	char *end;

	port = strtoul(s, &end, 0);
	if (*end != '\0') {
	    rb_raise(rb_eSocket, "no such servce %s/%s", s, proto);
	}
    }
    
    return INT2FIX(port);
}

static VALUE
sock_s_getaddrinfo(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE host, port, family, socktype, protocol, flags, ret;
    char hbuf[1024], pbuf[1024];
    char *hptr, *pptr;
    struct addrinfo hints, *res;
    int error;

    host = port = family = socktype = protocol = flags = Qnil;
    rb_scan_args(argc, argv, "24", &host, &port, &family, &socktype, &protocol,
		 &flags);
    if (NIL_P(host)) {
	hptr = NULL;
    }
    else {
	strncpy(hbuf, STR2CSTR(host), sizeof(hbuf));
	hbuf[sizeof(hbuf) - 1] = '\0';
	hptr = hbuf;
    }
    if (NIL_P(port)) {
	pptr = NULL;
    }
    else if (FIXNUM_P(port)) {
	snprintf(pbuf, sizeof(pbuf), "%d", FIX2INT(port));
	pptr = pbuf;
    }
    else {
	strncpy(pbuf, STR2CSTR(port), sizeof(pbuf));
	pbuf[sizeof(pbuf) - 1] = '\0';
	pptr = pbuf;
    }

    MEMZERO(&hints, struct addrinfo, 1);
    if (!NIL_P(family)) {
	hints.ai_family = NUM2INT(family);
    }
    else {
	hints.ai_family = PF_UNSPEC;
    }
    if (!NIL_P(socktype)) {
	hints.ai_socktype = NUM2INT(socktype);
    }
    if (!NIL_P(protocol)) {
	hints.ai_protocol = NUM2INT(protocol);
    }
    if (!NIL_P(flags)) {
	hints.ai_flags = NUM2INT(flags);
    }
    error = getaddrinfo(hptr, pptr, &hints, &res);
    if (error) {
	rb_raise(rb_eSocket, "%s", gai_strerror(error));
    }

    ret = mkaddrinfo(res);
    freeaddrinfo(res);
    return ret;
}

static VALUE
sock_s_getnameinfo(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE sa, af, host, port, flags;
    static char hbuf[1024], pbuf[1024];
    char *hptr, *pptr;
    int fl;
    struct addrinfo hints, *res = NULL;
    int error;
    struct sockaddr_storage ss;
    struct sockaddr *sap;

    sa = flags = Qnil;
    rb_scan_args(argc, argv, "11", &sa, &flags);

    if (TYPE(sa) == T_STRING) {
	if (sizeof(ss) < RSTRING(sa)->len) {
	    rb_raise(rb_eTypeError, "sockaddr length too big");
	}
	memcpy(&ss, RSTRING(sa)->ptr, RSTRING(sa)->len);
	if (RSTRING(sa)->len != SA_LEN((struct sockaddr *)&ss)) {
	    rb_raise(rb_eTypeError, "sockaddr size differs - should not happen");
	}
	sap = (struct sockaddr *)&ss;
    }
    else if (TYPE(sa) == T_ARRAY) {
	if (RARRAY(sa)->len == 3) {
	    af = RARRAY(sa)->ptr[0];
	    port = RARRAY(sa)->ptr[1];
	    host = RARRAY(sa)->ptr[2];
	}
	else if (RARRAY(sa)->len >= 4) {
	    af = RARRAY(sa)->ptr[0];
	    port = RARRAY(sa)->ptr[1];
	    host = RARRAY(sa)->ptr[3];
	    if (NIL_P(host)) {
		host = RARRAY(sa)->ptr[2];
	    }
	}
	if (NIL_P(host)) {
	    hptr = NULL;
	}
	else {
	    strncpy(hbuf, STR2CSTR(host), sizeof(hbuf));
	    hbuf[sizeof(hbuf) - 1] = '\0';
	    hptr = hbuf;
	}
	if (NIL_P(port)) {
	    strcpy(pbuf, "0");
	    pptr = NULL;
	}
	else if (!NIL_P(port)) {
	    snprintf(pbuf, sizeof(pbuf), "%d", NUM2INT(port));
	    pptr = pbuf;
	}
	else {
	    strncpy(pbuf, STR2CSTR(port), sizeof(pbuf));
	    pbuf[sizeof(pbuf) - 1] = '\0';
	    pptr = pbuf;
	}
	MEMZERO(&hints, struct addrinfo, 1);
	if (strcmp(STR2CSTR(af), "AF_INET") == 0) {
	    hints.ai_family = PF_INET;
	}
#ifdef INET6
	else if (strcmp(STR2CSTR(af), "AF_INET6") == 0) {
	    hints.ai_family = PF_INET6;
	}
#endif
	else {
	    hints.ai_family = PF_UNSPEC;
	}
	error = getaddrinfo(hptr, pptr, &hints, &res);
	if (error) {
	    rb_raise(rb_eSocket, "%s", gai_strerror(error));
	}
	sap = res->ai_addr;
    }
    else {
	rb_raise(rb_eTypeError, "expecting String or Array");
    }

    fl = 0;
    if (!NIL_P(flags)) {
	fl = NUM2INT(flags);
    }

  gotsap:
    error = getnameinfo(sap, SA_LEN(sap), hbuf, sizeof(hbuf),
			pbuf, sizeof(pbuf), fl);
    if (error) {
	rb_raise(rb_eSocket, "%s", gai_strerror(error));
    }
    if (res)
	freeaddrinfo(res);

    return rb_ary_new3(2, rb_str_new2(hbuf), rb_str_new2(pbuf));
}

static VALUE mConst;

static void
sock_define_const(name, value)
    char *name;
    int value;
{
    rb_define_const(rb_cSocket, name, INT2FIX(value));
    rb_define_const(mConst, name, INT2FIX(value));
}

void
Init_socket()
{
    rb_eSocket = rb_define_class("SocketError", rb_eStandardError);

    rb_cBasicSocket = rb_define_class("BasicSocket", rb_cIO);
    rb_undef_method(CLASS_OF(rb_cBasicSocket), "new");
    rb_undef_method(CLASS_OF(rb_cBasicSocket), "open");

    rb_define_singleton_method(rb_cBasicSocket, "do_not_reverse_lookup",
			       bsock_do_not_rev_lookup, 0);
    rb_define_singleton_method(rb_cBasicSocket, "do_not_reverse_lookup=",
			       bsock_do_not_rev_lookup_set, 1);

    rb_define_method(rb_cBasicSocket, "close_read", bsock_close_read, 0);
    rb_define_method(rb_cBasicSocket, "close_write", bsock_close_write, 0);
    rb_define_method(rb_cBasicSocket, "shutdown", bsock_shutdown, -1);
    rb_define_method(rb_cBasicSocket, "setsockopt", bsock_setsockopt, 3);
    rb_define_method(rb_cBasicSocket, "getsockopt", bsock_getsockopt, 2);
    rb_define_method(rb_cBasicSocket, "getsockname", bsock_getsockname, 0);
    rb_define_method(rb_cBasicSocket, "getpeername", bsock_getpeername, 0);
    rb_define_method(rb_cBasicSocket, "send", bsock_send, -1);
    rb_define_method(rb_cBasicSocket, "recv", bsock_recv, -1);

    rb_cIPSocket = rb_define_class("IPSocket", rb_cBasicSocket);
    rb_define_global_const("IPsocket", rb_cIPSocket);
    rb_define_method(rb_cIPSocket, "addr", ip_addr, 0);
    rb_define_method(rb_cIPSocket, "peeraddr", ip_peeraddr, 0);
    rb_define_singleton_method(rb_cIPSocket, "getaddress", ip_s_getaddress, 1);

    rb_cTCPSocket = rb_define_class("TCPSocket", rb_cIPSocket);
    rb_define_global_const("TCPsocket", rb_cTCPSocket);
    rb_define_singleton_method(rb_cTCPSocket, "open", tcp_s_open, 2);
    rb_define_singleton_method(rb_cTCPSocket, "new", tcp_s_open, 2);
    rb_define_singleton_method(rb_cTCPSocket, "gethostbyname", tcp_s_gethostbyname, 1);
    rb_define_method(rb_cTCPSocket, "recvfrom", tcp_recvfrom, -1);

#ifdef SOCKS
    rb_cSOCKSSocket = rb_define_class("SOCKSSocket", rb_cTCPSocket);
    rb_define_global_const("SOCKSsocket", rb_cSOCKSSocket);
    rb_define_singleton_method(rb_cSOCKSSocket, "open", socks_s_open, 2);
    rb_define_singleton_method(rb_cSOCKSSocket, "new", socks_s_open, 2);
#endif

    rb_cTCPServer = rb_define_class("TCPServer", rb_cTCPSocket);
    rb_define_global_const("TCPserver", rb_cTCPServer);
    rb_define_singleton_method(rb_cTCPServer, "open", tcp_svr_s_open, -1);
    rb_define_singleton_method(rb_cTCPServer, "new", tcp_svr_s_open, -1);
    rb_define_method(rb_cTCPServer, "accept", tcp_accept, 0);

    rb_cUDPSocket = rb_define_class("UDPSocket", rb_cIPSocket);
    rb_define_global_const("UDPsocket", rb_cUDPSocket);
    rb_define_singleton_method(rb_cUDPSocket, "open", udp_s_open, -1);
    rb_define_singleton_method(rb_cUDPSocket, "new", udp_s_open, -1);
    rb_define_method(rb_cUDPSocket, "connect", udp_connect, 2);
    rb_define_method(rb_cUDPSocket, "bind", udp_bind, 2);
    rb_define_method(rb_cUDPSocket, "send", udp_send, -1);
    rb_define_method(rb_cUDPSocket, "recvfrom", udp_recvfrom, -1);

#ifdef HAVE_SYS_UN_H
    rb_cUNIXSocket = rb_define_class("UNIXSocket", rb_cBasicSocket);
    rb_define_global_const("UNIXsocket", rb_cUNIXSocket);
    rb_define_singleton_method(rb_cUNIXSocket, "open", unix_s_sock_open, 1);
    rb_define_singleton_method(rb_cUNIXSocket, "new", unix_s_sock_open, 1);
    rb_define_method(rb_cUNIXSocket, "path", unix_path, 0);
    rb_define_method(rb_cUNIXSocket, "addr", unix_addr, 0);
    rb_define_method(rb_cUNIXSocket, "peeraddr", unix_peeraddr, 0);
    rb_define_method(rb_cUNIXSocket, "recvfrom", unix_recvfrom, -1);

    rb_cUNIXServer = rb_define_class("UNIXServer", rb_cUNIXSocket);
    rb_define_global_const("UNIXserver", rb_cUNIXServer);
    rb_define_singleton_method(rb_cUNIXServer, "open", unix_svr_s_open, 1);
    rb_define_singleton_method(rb_cUNIXServer, "new", unix_svr_s_open, 1);
    rb_define_method(rb_cUNIXServer, "accept", unix_accept, 0);
#endif

    rb_cSocket = rb_define_class("Socket", rb_cBasicSocket);
    rb_define_singleton_method(rb_cSocket, "open", sock_s_open, 3);
    rb_define_singleton_method(rb_cSocket, "new", sock_s_open, 3);
    rb_define_singleton_method(rb_cSocket, "for_fd", sock_s_for_fd, 1);

    rb_define_method(rb_cSocket, "connect", sock_connect, 1);
    rb_define_method(rb_cSocket, "bind", sock_bind, 1);
    rb_define_method(rb_cSocket, "listen", sock_listen, 1);
    rb_define_method(rb_cSocket, "accept", sock_accept, 0);

    rb_define_method(rb_cSocket, "recvfrom", sock_recvfrom, -1);

    rb_define_singleton_method(rb_cSocket, "socketpair", sock_s_socketpair, 3);
    rb_define_singleton_method(rb_cSocket, "pair", sock_s_socketpair, 3);
    rb_define_singleton_method(rb_cSocket, "gethostname", sock_gethostname, 0);
    rb_define_singleton_method(rb_cSocket, "gethostbyname", sock_s_gethostbyname, 1);
    rb_define_singleton_method(rb_cSocket, "gethostbyaddr", sock_s_gethostbyaddr, -1);
    rb_define_singleton_method(rb_cSocket, "getservbyname", sock_s_getservbyaname, -1);
    rb_define_singleton_method(rb_cSocket, "getaddrinfo", sock_s_getaddrinfo, -1);
    rb_define_singleton_method(rb_cSocket, "getnameinfo", sock_s_getnameinfo, -1);

    /* constants */
    mConst = rb_define_module_under(rb_cSocket, "Constants");
    sock_define_const("SOCK_STREAM", SOCK_STREAM);
    sock_define_const("SOCK_DGRAM", SOCK_DGRAM);
#ifdef SOCK_RAW
    sock_define_const("SOCK_RAW", SOCK_RAW);
#endif
#ifdef SOCK_RDM
    sock_define_const("SOCK_RDM", SOCK_RDM);
#endif
#ifdef SOCK_SEQPACKET
    sock_define_const("SOCK_SEQPACKET", SOCK_SEQPACKET);
#endif
#ifdef SOCK_PACKET
    sock_define_const("SOCK_PACKET", SOCK_PACKET);
#endif

    sock_define_const("AF_INET", AF_INET);
#ifdef PF_INET
    sock_define_const("PF_INET", PF_INET);
#endif
#ifdef AF_UNIX
    sock_define_const("AF_UNIX", AF_UNIX);
    sock_define_const("PF_UNIX", PF_UNIX);
#endif
#ifdef AF_AX25
    sock_define_const("AF_AX25", AF_AX25);
    sock_define_const("PF_AX25", PF_AX25);
#endif
#ifdef AF_IPX
    sock_define_const("AF_IPX", AF_IPX);
    sock_define_const("PF_IPX", PF_IPX);
#endif
#ifdef AF_APPLETALK
    sock_define_const("AF_APPLETALK", AF_APPLETALK);
    sock_define_const("PF_APPLETALK", PF_APPLETALK);
#endif
#ifdef AF_UNSPEC
    sock_define_const("AF_UNSPEC", AF_UNSPEC);
    sock_define_const("PF_UNSPEC", PF_UNSPEC);
#endif
#ifdef AF_INET6
    sock_define_const("AF_INET6", AF_INET6);
#endif
#ifdef PF_INET6
    sock_define_const("PF_INET6", PF_INET6);
#endif

    sock_define_const("MSG_OOB", MSG_OOB);
#ifdef MSG_PEEK
    sock_define_const("MSG_PEEK", MSG_PEEK);
#endif
#ifdef MSG_DONTROUTE
    sock_define_const("MSG_DONTROUTE", MSG_DONTROUTE);
#endif

    sock_define_const("SOL_SOCKET", SOL_SOCKET);
#ifdef SOL_IP
    sock_define_const("SOL_IP", SOL_IP);
#endif
#ifdef SOL_IPX
    sock_define_const("SOL_IPX", SOL_IPX);
#endif
#ifdef SOL_AX25
    sock_define_const("SOL_AX25", SOL_AX25);
#endif
#ifdef SOL_ATALK
    sock_define_const("SOL_ATALK", SOL_ATALK);
#endif
#ifdef SOL_TCP
    sock_define_const("SOL_TCP", SOL_TCP);
#endif
#ifdef SOL_UDP
    sock_define_const("SOL_UDP", SOL_UDP);
#endif

#ifdef SO_DEBUG
    sock_define_const("SO_DEBUG", SO_DEBUG);
#endif
    sock_define_const("SO_REUSEADDR", SO_REUSEADDR);
#ifdef SO_TYPE
    sock_define_const("SO_TYPE", SO_TYPE);
#endif
#ifdef SO_ERROR
    sock_define_const("SO_ERROR", SO_ERROR);
#endif
#ifdef SO_DONTROUTE
    sock_define_const("SO_DONTROUTE", SO_DONTROUTE);
#endif
#ifdef SO_BROADCAST
    sock_define_const("SO_BROADCAST", SO_BROADCAST);
#endif
#ifdef SO_SNDBUF
    sock_define_const("SO_SNDBUF", SO_SNDBUF);
#endif
#ifdef SO_RCVBUF
    sock_define_const("SO_RCVBUF", SO_RCVBUF);
#endif
#ifdef SO_KEEPALIVE
    sock_define_const("SO_KEEPALIVE", SO_KEEPALIVE);
#endif
#ifdef SO_OOBINLINE
    sock_define_const("SO_OOBINLINE", SO_OOBINLINE);
#endif
#ifdef SO_NO_CHECK
    sock_define_const("SO_NO_CHECK", SO_NO_CHECK);
#endif
#ifdef SO_PRIORITY
    sock_define_const("SO_PRIORITY", SO_PRIORITY);
#endif
#ifdef SO_LINGER
    sock_define_const("SO_LINGER", SO_LINGER);
#endif

#ifdef SOPRI_INTERACTIVE
    sock_define_const("SOPRI_INTERACTIVE", SOPRI_INTERACTIVE);
#endif
#ifdef SOPRI_NORMAL
    sock_define_const("SOPRI_NORMAL", SOPRI_NORMAL);
#endif
#ifdef SOPRI_BACKGROUND
    sock_define_const("SOPRI_BACKGROUND", SOPRI_BACKGROUND);
#endif

#ifdef IP_MULTICAST_IF
    sock_define_const("IP_MULTICAST_IF", IP_MULTICAST_IF);
#endif
#ifdef IP_MULTICAST_TTL
    sock_define_const("IP_MULTICAST_TTL", IP_MULTICAST_TTL);
#endif
#ifdef IP_MULTICAST_LOOP
    sock_define_const("IP_MULTICAST_LOOP", IP_MULTICAST_LOOP);
#endif
#ifdef IP_ADD_MEMBERSHIP
    sock_define_const("IP_ADD_MEMBERSHIP", IP_ADD_MEMBERSHIP);
#endif

#ifdef IP_DEFAULT_MULTICAST_TTL
    sock_define_const("IP_DEFAULT_MULTICAST_TTL", IP_DEFAULT_MULTICAST_TTL);
#endif
#ifdef IP_DEFAULT_MULTICAST_LOOP
    sock_define_const("IP_DEFAULT_MULTICAST_LOOP", IP_DEFAULT_MULTICAST_LOOP);
#endif
#ifdef IP_MAX_MEMBERSHIPS
    sock_define_const("IP_MAX_MEMBERSHIPS", IP_MAX_MEMBERSHIPS);
#endif

#ifdef IPX_TYPE
    sock_define_const("IPX_TYPE", IPX_TYPE);
#endif

#ifdef TCP_NODELAY
    sock_define_const("TCP_NODELAY", TCP_NODELAY);
#endif
#ifdef TCP_MAXSEG
    sock_define_const("TCP_MAXSEG", TCP_MAXSEG);
#endif

#ifdef EAI_ADDRFAMILY
    sock_define_const("EAI_ADDRFAMILY", EAI_ADDRFAMILY);
#endif
#ifdef EAI_AGAIN
    sock_define_const("EAI_AGAIN", EAI_AGAIN);
#endif
#ifdef EAI_BADFLAGS
    sock_define_const("EAI_BADFLAGS", EAI_BADFLAGS);
#endif
#ifdef EAI_FAIL
    sock_define_const("EAI_FAIL", EAI_FAIL);
#endif
#ifdef EAI_FAMILY
    sock_define_const("EAI_FAMILY", EAI_FAMILY);
#endif
#ifdef EAI_MEMORY
    sock_define_const("EAI_MEMORY", EAI_MEMORY);
#endif
#ifdef EAI_NODATA
    sock_define_const("EAI_NODATA", EAI_NODATA);
#endif
#ifdef EAI_NONAME
    sock_define_const("EAI_NONAME", EAI_NONAME);
#endif
#ifdef EAI_SERVICE
    sock_define_const("EAI_SERVICE", EAI_SERVICE);
#endif
#ifdef EAI_SOCKTYPE
    sock_define_const("EAI_SOCKTYPE", EAI_SOCKTYPE);
#endif
#ifdef EAI_SYSTEM
    sock_define_const("EAI_SYSTEM", EAI_SYSTEM);
#endif
#ifdef EAI_BADHINTS
    sock_define_const("EAI_BADHINTS", EAI_BADHINTS);
#endif
#ifdef EAI_PROTOCOL
    sock_define_const("EAI_PROTOCOL", EAI_PROTOCOL);
#endif
#ifdef EAI_MAX
    sock_define_const("EAI_MAX", EAI_MAX);
#endif
#ifdef AI_PASSIVE
    sock_define_const("AI_PASSIVE", AI_PASSIVE);
#endif
#ifdef AI_CANONNAME
    sock_define_const("AI_CANONNAME", AI_CANONNAME);
#endif
#ifdef AI_NUMERICHOST
    sock_define_const("AI_NUMERICHOST", AI_NUMERICHOST);
#endif
#ifdef AI_MASK
    sock_define_const("AI_MASK", AI_MASK);
#endif
#ifdef AI_ALL
    sock_define_const("AI_ALL", AI_ALL);
#endif
#ifdef AI_V4MAPPED_CFG
    sock_define_const("AI_V4MAPPED_CFG", AI_V4MAPPED_CFG);
#endif
#ifdef AI_ADDRCONFIG
    sock_define_const("AI_ADDRCONFIG", AI_ADDRCONFIG);
#endif
#ifdef AI_V4MAPPED
    sock_define_const("AI_V4MAPPED", AI_V4MAPPED);
#endif
#ifdef AI_DEFAULT
    sock_define_const("AI_DEFAULT", AI_DEFAULT);
#endif
#ifdef NI_MAXHOST
    sock_define_const("NI_MAXHOST", NI_MAXHOST);
#endif
#ifdef NI_MAXSERV
    sock_define_const("NI_MAXSERV", NI_MAXSERV);
#endif
#ifdef NI_NOFQDN
    sock_define_const("NI_NOFQDN", NI_NOFQDN);
#endif
#ifdef NI_NUMERICHOST
    sock_define_const("NI_NUMERICHOST", NI_NUMERICHOST);
#endif
#ifdef NI_NAMEREQD
    sock_define_const("NI_NAMEREQD", NI_NAMEREQD);
#endif
#ifdef NI_NUMERICSERV
    sock_define_const("NI_NUMERICSERV", NI_NUMERICSERV);
#endif
#ifdef NI_DGRAM
    sock_define_const("NI_DGRAM", NI_DGRAM);
#endif
}
