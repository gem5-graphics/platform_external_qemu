/*
 * Copyright (c) 1995 Danny Gasparovski.
 *
 * Please read the file COPYRIGHT for the
 * terms and conditions of the copyright.
 */

#define WANT_SYS_IOCTL_H
#include "qemu-common.h"
#include <slirp.h>
#include "ip_icmp.h"
#include "main.h"
#ifdef __sun__
#include <sys/filio.h>
#endif
#define  SLIRP_COMPILATION 1
#include "android/proxy/proxy_common.h"
#include "android/utils/sockets.h"

static void sofcantrcvmore(struct socket *so);
static void sofcantsendmore(struct socket *so);

#if 0
static void
so_init()
{
	/* Nothing yet */
}
#endif

struct socket *
solookup(struct socket **last, struct socket *head,
         SockAddress* laddr, SockAddress* faddr)
{
    struct socket *so = *last;

    /* Optimization */
    if (so != head && sock_address_equal(&so->laddr, laddr) &&
        (!faddr || sock_address_equal(&so->faddr, faddr))) {
        return so;
    }

    for (so = head->so_next; so != head; so = so->so_next) {
        if (sock_address_equal(&so->laddr, laddr) &&
            (!faddr || sock_address_equal(&so->faddr, faddr))) {
            *last = so;
            return so;
        }
    }
    return NULL;
}

/*
 * Create a new socket, initialise the fields
 * It is the responsibility of the caller to
 * insque() it into the correct linked-list
 */
struct socket *
socreate(void)
{
  struct socket *so;

  so = (struct socket *)malloc(sizeof(struct socket));
  if(so) {
    memset(so, 0, sizeof(struct socket));
    so->so_state = SS_NOFDREF;
    so->s = -1;
  }
  return(so);
}

/*
 * remque and free a socket, clobber cache
 */
void
sofree(struct socket *so)
{
  if (so->so_state & SS_PROXIFIED)
    proxy_manager_del(so);

  if (so->so_emu==EMU_RSH && so->extra) {
	sofree(so->extra);
	so->extra=NULL;
  }
  if (so == tcp_last_so)
    tcp_last_so = &tcb;
  else if (so == udp_last_so)
    udp_last_so = &udb;

  m_free(so->so_m);

  if(so->so_next && so->so_prev)
    remque(so);  /* crashes if so is not in a queue */

  free(so);
}

size_t sopreprbuf(struct socket *so, struct iovec *iov, int *np)
{
	int n, lss, total;
	struct sbuf *sb = &so->so_snd;
	int len = sb->sb_datalen - sb->sb_cc;
	int mss = so->so_tcpcb->t_maxseg;

	DEBUG_CALL("sopreprbuf");
	DEBUG_ARG("so = %lx", (long )so);

	len = sb->sb_datalen - sb->sb_cc;

	if (len <= 0)
		return 0;

	iov[0].iov_base = sb->sb_wptr;
        iov[1].iov_base = NULL;
        iov[1].iov_len = 0;
	if (sb->sb_wptr < sb->sb_rptr) {
		iov[0].iov_len = sb->sb_rptr - sb->sb_wptr;
		/* Should never succeed, but... */
		if (iov[0].iov_len > (size_t)len)
		   iov[0].iov_len = (size_t)len;
		if (iov[0].iov_len > (size_t)mss)
		   iov[0].iov_len -= iov[0].iov_len%mss;
		n = 1;
	} else {
		iov[0].iov_len = (sb->sb_data + sb->sb_datalen) - sb->sb_wptr;
		/* Should never succeed, but... */
		if (iov[0].iov_len > (size_t)len) iov[0].iov_len = (size_t)len;
		len -= iov[0].iov_len;
		if (len) {
			iov[1].iov_base = sb->sb_data;
			iov[1].iov_len = sb->sb_rptr - sb->sb_data;
			if(iov[1].iov_len > (size_t)len)
			   iov[1].iov_len = (size_t)len;
			total = iov[0].iov_len + iov[1].iov_len;
			if (total > mss) {
				lss = total%mss;
				if (iov[1].iov_len > (size_t)lss) {
					iov[1].iov_len -= (size_t)lss;
					n = 2;
				} else {
					lss -= iov[1].iov_len;
					iov[0].iov_len -= lss;
					n = 1;
				}
			} else
				n = 2;
		} else {
			if (iov[0].iov_len > (size_t)mss)
			   iov[0].iov_len -= iov[0].iov_len%mss;
			n = 1;
		}
	}
	if (np)
		*np = n;

	return iov[0].iov_len + (n - 1) * iov[1].iov_len;
}

/*
 * Read from so's socket into sb_snd, updating all relevant sbuf fields
 * NOTE: This will only be called if it is select()ed for reading, so
 * a read() of 0 (or less) means it's disconnected
 */
int
soread(struct socket *so)
{
    int n = 0, nn;
	struct sbuf *sb = &so->so_snd;
    struct iovec iov[2] = {};

	DEBUG_CALL("soread");
	DEBUG_ARG("so = %lx", (long )so);

	/*
	 * No need to check if there's enough room to read.
	 * soread wouldn't have been called if there weren't
	 */
	sopreprbuf(so, iov, &n);

#ifdef HAVE_READV
	nn = readv(so->s, (struct iovec *)iov, n);
	DEBUG_MISC((dfd, " ... read nn = %d bytes\n", nn));
#else
	nn = socket_recv(so->s, iov[0].iov_base, iov[0].iov_len);
#endif
	if (nn <= 0) {
		if (nn < 0 && (errno == EINTR || errno == EAGAIN))
			return 0;
		else {
			DEBUG_MISC((dfd, " --- soread() disconnected, nn = %d, errno = %d-%s\n", nn, errno,errno_str));
			sofcantrcvmore(so);
			tcp_sockclosed(sototcpcb(so));
			return -1;
		}
	}

#ifndef HAVE_READV
	/*
	 * If there was no error, try and read the second time round
	 * We read again if n = 2 (ie, there's another part of the buffer)
	 * and we read as much as we could in the first read
	 * We don't test for <= 0 this time, because there legitimately
	 * might not be any more data (since the socket is non-blocking),
	 * a close will be detected on next iteration.
	 * A return of -1 wont (shouldn't) happen, since it didn't happen above
	 */
	if (n == 2 && (size_t)nn == iov[0].iov_len) {
            int ret;
            ret = socket_recv(so->s, iov[1].iov_base, iov[1].iov_len);
            if (ret > 0)
                nn += ret;
        }

	DEBUG_MISC((dfd, " ... read nn = %d bytes\n", nn));
#endif

	/* Update fields */
	sb->sb_cc += nn;
	sb->sb_wptr += nn;
	if (sb->sb_wptr >= (sb->sb_data + sb->sb_datalen))
		sb->sb_wptr -= sb->sb_datalen;
	return nn;
}

int soreadbuf(struct socket *so, const char *buf, int size)
{
    int n = 0, nn, copy = size;
	struct sbuf *sb = &so->so_snd;
    struct iovec iov[2] = {};

	DEBUG_CALL("soreadbuf");
	DEBUG_ARG("so = %lx", (long )so);

	/*
	 * No need to check if there's enough room to read.
	 * soread wouldn't have been called if there weren't
	 */
	if (sopreprbuf(so, iov, &n) < (size_t)size)
 
        goto err;

    nn = MIN(iov[0].iov_len, (size_t)copy);
    memcpy(iov[0].iov_base, buf, nn);

    copy -= nn;
    buf += nn;

    if (copy == 0)
        goto done;

    memcpy(iov[1].iov_base, buf, copy);

done:
    /* Update fields */
	sb->sb_cc += size;
	sb->sb_wptr += size;
	if (sb->sb_wptr >= (sb->sb_data + sb->sb_datalen))
		sb->sb_wptr -= sb->sb_datalen;
    return size;
err:

    sofcantrcvmore(so);
    tcp_sockclosed(sototcpcb(so));
    fprintf(stderr, "soreadbuf buffer to small");
    return -1;
}

/*
 * Get urgent data
 *
 * When the socket is created, we set it SO_OOBINLINE,
 * so when OOB data arrives, we soread() it and everything
 * in the send buffer is sent as urgent data
 */
void
sorecvoob(struct socket *so)
{
	struct tcpcb *tp = sototcpcb(so);

	DEBUG_CALL("sorecvoob");
	DEBUG_ARG("so = %lx", (long)so);

	/*
	 * We take a guess at how much urgent data has arrived.
	 * In most situations, when urgent data arrives, the next
	 * read() should get all the urgent data.  This guess will
	 * be wrong however if more data arrives just after the
	 * urgent data, or the read() doesn't return all the
	 * urgent data.
	 */
	soread(so);
	tp->snd_up = tp->snd_una + so->so_snd.sb_cc;
	tp->t_force = 1;
	tcp_output(tp);
	tp->t_force = 0;
}

/*
 * Send urgent data
 * There's a lot duplicated code here, but...
 */
int
sosendoob(struct socket *so)
{
	struct sbuf *sb = &so->so_rcv;
	char buff[2048]; /* XXX Shouldn't be sending more oob data than this */

	int n, len;

	DEBUG_CALL("sosendoob");
	DEBUG_ARG("so = %lx", (long)so);
	DEBUG_ARG("sb->sb_cc = %d", sb->sb_cc);

	if (so->so_urgc > 2048)
	   so->so_urgc = 2048; /* XXXX */

	if (sb->sb_rptr < sb->sb_wptr) {
		/* We can send it directly */
		n = socket_send_oob(so->s, sb->sb_rptr, so->so_urgc); /* |MSG_DONTWAIT)); */
		so->so_urgc -= n;

		DEBUG_MISC((dfd, " --- sent %d bytes urgent data, %d urgent bytes left\n", n, so->so_urgc));
	} else {
		/*
		 * Since there's no sendv or sendtov like writev,
		 * we must copy all data to a linear buffer then
		 * send it all
		 */
		len = (sb->sb_data + sb->sb_datalen) - sb->sb_rptr;
		if (len > so->so_urgc) len = so->so_urgc;
		memcpy(buff, sb->sb_rptr, len);
		so->so_urgc -= len;
		if (so->so_urgc) {
			n = sb->sb_wptr - sb->sb_data;
			if (n > so->so_urgc) n = so->so_urgc;
			memcpy((buff + len), sb->sb_data, n);
			so->so_urgc -= n;
			len += n;
		}
		n = socket_send_oob(so->s, buff, len); /* |MSG_DONTWAIT)); */
#ifdef DEBUG
		if (n != len)
		   DEBUG_ERROR((dfd, "Didn't send all data urgently XXXXX\n"));
#endif
		DEBUG_MISC((dfd, " ---2 sent %d bytes urgent data, %d urgent bytes left\n", n, so->so_urgc));
	}

	sb->sb_cc -= n;
	sb->sb_rptr += n;
	if (sb->sb_rptr >= (sb->sb_data + sb->sb_datalen))
		sb->sb_rptr -= sb->sb_datalen;

	return n;
}

/*
 * Write data from so_rcv to so's socket,
 * updating all sbuf field as necessary
 */
int
sowrite(struct socket *so)
{
	int  n,nn;
	struct sbuf *sb = &so->so_rcv;
	int len = sb->sb_cc;
	struct iovec iov[2];

	DEBUG_CALL("sowrite");
	DEBUG_ARG("so = %lx", (long)so);

	if (so->so_urgc) {
		sosendoob(so);
		if (sb->sb_cc == 0)
			return 0;
	}

	/*
	 * No need to check if there's something to write,
	 * sowrite wouldn't have been called otherwise
	 */

        len = sb->sb_cc;

	iov[0].iov_base = sb->sb_rptr;
        iov[1].iov_base = NULL;
        iov[1].iov_len = 0;
	if (sb->sb_rptr < sb->sb_wptr) {
		iov[0].iov_len = sb->sb_wptr - sb->sb_rptr;
		/* Should never succeed, but... */
		if (iov[0].iov_len > (size_t)len) iov[0].iov_len = (size_t)len;
		n = 1;
	} else {
		iov[0].iov_len = (sb->sb_data + sb->sb_datalen) - sb->sb_rptr;
		if (iov[0].iov_len > (size_t)len) iov[0].iov_len = (size_t)len;
		len -= iov[0].iov_len;
		if (len) {
			iov[1].iov_base = sb->sb_data;
			iov[1].iov_len = sb->sb_wptr - sb->sb_data;
			if (iov[1].iov_len > (size_t)len) iov[1].iov_len = (size_t)len;
			n = 2;
		} else
			n = 1;
	}
	/* Check if there's urgent data to send, and if so, send it */

#ifdef HAVE_READV
	nn = writev(so->s, (const struct iovec *)iov, n);

	DEBUG_MISC((dfd, "  ... wrote nn = %d bytes\n", nn));
#else
	nn = socket_send(so->s, iov[0].iov_base, iov[0].iov_len);
#endif
	/* This should never happen, but people tell me it does *shrug* */
	if (nn < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;

	if (nn <= 0) {
		DEBUG_MISC((dfd, " --- sowrite disconnected, so->so_state = %x, errno = %d\n",
			so->so_state, errno));
		sofcantsendmore(so);
		tcp_sockclosed(sototcpcb(so));
		return -1;
	}

#ifndef HAVE_READV
	if (n == 2 && (size_t)nn == iov[0].iov_len) {
            int ret;
            ret = socket_send(so->s, iov[1].iov_base, iov[1].iov_len);
            if (ret > 0)
                nn += ret;
        }
        DEBUG_MISC((dfd, "  ... wrote nn = %d bytes\n", nn));
#endif

	/* Update sbuf */
	sb->sb_cc -= nn;
	sb->sb_rptr += nn;
	if (sb->sb_rptr >= (sb->sb_data + sb->sb_datalen))
		sb->sb_rptr -= sb->sb_datalen;

	/*
	 * If in DRAIN mode, and there's no more data, set
	 * it CANTSENDMORE
	 */
	if ((so->so_state & SS_FWDRAIN) && sb->sb_cc == 0)
		sofcantsendmore(so);

	return nn;
}

/* The domain name in dns packet is encoded like this:
 * "a.bc.com" -> {0x1, 'a', 0x2, 'b', 'c', 0x3, 'c', 'o', 'm', 0x0}
 */
static void enc_dns(const char* src, char* dst, int size) {
    if (strlen(src) + 2 != size) {
        g_assert_not_reached();
    }
    strcpy(dst + 1, src);
    char* p = dst;
    char* tmp;
    for (;;) {
        tmp = strchr(p + 1, '.');
        if (tmp) {
            *tmp = 0;
        }
        *p = strlen(p + 1);
        if (NULL == tmp)
            break;
        p = tmp;
    }
}

static const char old_aserver[] = "android.clients.google.com";
/* we use a fake domain name which has same length with old one
 * so we don't need to parse the dns packet to fix offset issue.
 * It's lucky that it's resolvable.
 */
static const char new_aserver[] = "androidhack.googleapis.com";
static char old_aserver_enc[sizeof(old_aserver) + 1];
static char new_aserver_enc[sizeof(new_aserver) + 1];

void init_aserver_enc() {
    static int done = 0;
    if (done)
        return;
    enc_dns(old_aserver, old_aserver_enc, sizeof(old_aserver_enc));
    enc_dns(new_aserver, new_aserver_enc, sizeof(new_aserver_enc));
    done = 1;
}

static const unsigned short kDnsPort = 53;
/* https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml */
static const unsigned short kTypeAAAA = 28;

/* This is used to replace DNS query for android.clients.google.com
 * to androidhack.googleapis.com since the previous one is not accessible
 * from pure IPv6 env */
static void dns_hack(struct socket* so,
                     struct mbuf* m,
                     const char* src_dns,
                     int src_len,
                     const char* dst_dns,
                     int dst_len) {
    if (!slirp_hack_dns)
        return;
    if (sock_address_get_port(&so->faddr) != kDnsPort || so->so_family == SOCKET_INET)
        return;
    init_aserver_enc();
    if (src_len != dst_len) {
        g_assert_not_reached();
    }
    /* The name starts from bytes 12 */
    const int dns_name_off = 12;
    if (m->m_len < dns_name_off + src_len + 2)
        return;
    char* name = m->m_data + dns_name_off;
    if (memcmp(name, src_dns, src_len))
        return;
    unsigned short type = (name[src_len] << 8)  +  name[src_len + 1];
    if (type != kTypeAAAA)
        return;
    memcpy(name, dst_dns, dst_len);
    return;
}

/*
 * recvfrom() a UDP socket
 */
void
sorecvfrom(struct socket *so)
{
        SockAddress  addr;

	DEBUG_CALL("sorecvfrom");
	DEBUG_ARG("so = %lx", (long)so);

	if (so->so_type == IPPROTO_ICMP) {   /* This is a "ping" reply */
	  char buff[256];
	  int len;

	  len = socket_recvfrom(so->s, buff, 256, &addr);
	  /* XXX Check if reply is "correct"? */

	  if(len == -1 || len == 0) {
	    u_char code=ICMP_UNREACH_PORT;

	    if(errno == EHOSTUNREACH) code=ICMP_UNREACH_HOST;
	    else if(errno == ENETUNREACH) code=ICMP_UNREACH_NET;

	    DEBUG_MISC((dfd," udp icmp rx errno = %d-%s\n",
			errno,errno_str));
	    icmp_error(so->so_m, ICMP_UNREACH,code, 0,errno_str);
	  } else {
	    icmp_reflect(so->so_m);
	    so->so_m = 0; /* Don't mbuf_free() it again! */
	  }
	  /* No need for this socket anymore, udp_detach it */
	  udp_detach(so);
	} else {                            	/* A "normal" UDP packet */
	  struct mbuf *m;
          int len;
		  int n;

	  if (!(m = m_get())) return;
	  m->m_data += IF_MAXLINKHDR + sizeof(struct udphdr);
	  switch (so->faddr.family) {
	  case SOCKET_INET:
	    m->m_data += sizeof(struct ip);
	    break;
	  case SOCKET_IN6:
	    m->m_data += sizeof(struct ip6);
	    break;
	  default:
	    g_assert_not_reached();
	    break;
	  }
	  /*
	   * XXX Shouldn't FIONREAD packets destined for port 53,
	   * but I don't know the max packet size for DNS lookups
	   */
	  len = M_FREEROOM(m);
	  /* if (so->so_fport != htons(53)) { */
	  n = socket_can_read(so->s);

	  if (n > len) {
	    n = (m->m_data - m->m_dat) + m->m_len + n + 1;
	    m_inc(m, n);
	    len = M_FREEROOM(m);
	  }
	  /* } */

	  m->m_len = socket_recvfrom(so->s, m->m_data, len, &addr);

	  /* restore possible modified DNS replies in IPv6 mode */
	  dns_hack(so, m, new_aserver_enc, sizeof(new_aserver_enc),
			  old_aserver_enc, sizeof(old_aserver_enc));

	  DEBUG_MISC((dfd, " did recvfrom %d, errno = %d-%s\n",
		      m->m_len, errno,errno_str));
	  if(m->m_len<0) {
	    switch (so->laddr.family) {
	    u_char code;
	    case SOCKET_INET:
		code = ICMP_UNREACH_PORT;

		if (errno == EHOSTUNREACH)
		    code = ICMP_UNREACH_HOST;
		else if (errno == ENETUNREACH)
		    code = ICMP_UNREACH_NET;

		DEBUG_MISC((dfd, " rx error, tx icmp ICMP_UNREACH:%i\n",
		            code));
		icmp_error(so->so_m, ICMP_UNREACH, code, 0, errno_str);
		break;
	    case SOCKET_IN6:
		code = ICMP6_UNREACH_PORT;

		if (errno == EHOSTUNREACH)
		    code = ICMP6_UNREACH_ADDRESS;
		else if (errno == ENETUNREACH)
		    code = ICMP6_UNREACH_NO_ROUTE;

		DEBUG_MISC((dfd, " rx error, tx icmp6 ICMP_UNREACH:%i\n",
		            code));
		icmp6_send_error(so->so_m, ICMP6_UNREACH, code);
		break;
	    default:
		g_assert_not_reached();
		break;
	    }
	    m_free(m);
	  } else {
	  /*
	   * Hack: domain name lookup will be used the most for UDP,
	   * and since they'll only be used once there's no need
	   * for the 4 minute (or whatever) timeout... So we time them
	   * out much quicker (10 seconds  for now...)
	   */
	    if (so->so_expire) {
	      if (so->so_faddr_port == kDnsPort)
		so->so_expire = curtime + SO_EXPIREFAST;
	      else
		so->so_expire = curtime + SO_EXPIRE;
	    }

	    /*		if (m->m_len == len) {
	     *			m_inc(m, MINCSIZE);
	     *			m->m_len = 0;
	     *		}
	     */

	    switch (so->faddr.family) {
	    struct sockaddr_in6 saddr, daddr;
	    case SOCKET_INET:
		/*
		 * If this packet was destined for CTL_ADDR,
		 * make it look like that's where it came from, done by
		 * udp_output
		 */
		udp_output_(so, m, &addr);
		break;
	    case SOCKET_IN6:
		saddr.sin6_port = htons(sock_address_get_port(&addr));
		memcpy(saddr.sin6_addr.s6_addr, addr.u.in6.address, 16);
		/*
		 * If this packet was destined for CTL_ADDR,
		 * make it look like that's where it came from */
		if (in6_equal_net((struct in6_addr*)&so->faddr.u.in6.address,
				  &vprefix_addr6, vprefix_len)) {
		    memcpy(saddr.sin6_addr.s6_addr, so->faddr.u.in6.address,
		           16);
		}
		daddr.sin6_port = htons(sock_address_get_port(&so->laddr));
		memcpy(daddr.sin6_addr.s6_addr, so->laddr.u.in6.address,
		       16);
		udp6_output(so, m, &saddr, &daddr);
		break;
	    default:
		g_assert_not_reached();
	    }
	  } /* rx error */
	} /* if ping packet */
}

static void set_local_address(SockAddress* addr, uint16_t port,
			      SocketFamily sf)
{
  switch (sf) {
  case SOCKET_INET:
    sock_address_init_inet(addr, loopback_addr_ip, port);
    break;
  case SOCKET_IN6:
    sock_address_init_in6_loopback(addr, port);
    break;
  default:
    g_assert_not_reached();
  }
}

/*
 * Translate addr in host addr when it is a virtual address
 */
int sotranslate_out(struct socket *so, SockAddress *addr)
{
    uint16_t port = sock_address_get_port(addr);
    switch (addr->family) {
    case SOCKET_INET:
        if ((so->faddr.u.inet.address & 0xffffff00) == special_addr_ip) {
            /* It's an alias */
            int  low = so->faddr.u.inet.address & 0xff;
            if ( CTL_IS_DNS(low) ) {
               *addr = dns_addr[low - CTL_DNS];
               sock_address_set_port(addr, port);
            } else {
               set_local_address(addr, port, so->so_family);
            }
            return 1;
        }
        break;
    case SOCKET_IN6:
        if (in6_equal_net((struct in6_addr*) &so->faddr.u.in6.address,
            &vprefix_addr6, vprefix_len)) {
            set_local_address(addr, port, so->so_family);
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}

/* Ensure so->s's domain is |af|, if not, re-create a new socket instance.
 * Do not change the state of |so| otherwise. In practice, this is used
 * when the guest DNS IPv4 address is translated into a host IPv6 one:
 * if this happens, the socket must change from AF_INET to AF_INET6 for
 * sendto() to work correctly. See sosendto(). */
static void
udp_reattach(struct socket *so, SocketFamily sf)
{
    if (so->s != -1) {
        if (so->so_family == sf) {
            /* Nothing to reattach. */
            return;
        }
        socket_close(so->s);
    }
    so->so_family = sf;
    so->s = socket_create(sf, SOCKET_DGRAM);
    return;
}

/*
 * sendto() a socket
 */
int
sosendto(struct socket *so, struct mbuf *m)
{
	int ret;
	SockAddress addr = so->faddr;

	DEBUG_CALL("sosendto");
	DEBUG_ARG("so = %lx", (long)so);
	DEBUG_ARG("m = %lx", (long)m);

	sotranslate_out(so, &addr);

	/*
	 * test for generic forwarding; this function replaces the arguments
	 * only on success
	 */
	unsigned long faddr = sock_address_get_ip(&addr);
	int fport = sock_address_get_port(&addr);

	if (slirp_should_net_forward(faddr, fport, &faddr, &fport)) {
      time_t timestamp = time(NULL);
      char tmp[256];
      slirp_drop_log(
	       "Redirected UDP: src: 0x%08lx:0x%04x org dst: %s "
	       "new dst: 0x%08lx:0x%04x %ld\n",
	        so->so_laddr_ip, so->so_laddr_port,
	        sock_address_to_string(&addr, tmp, sizeof(tmp)),
	        faddr, fport, timestamp
	    );
            sock_address_init_inet(&addr, faddr, fport);
	}

	DEBUG_MISC((dfd, " sendto()ing, %s\n", sock_address_to_string(&addr)));

	udp_reattach(so, addr.family);
	/* replace DNS queries in IPv6 mode */
	dns_hack(so, m, old_aserver_enc, sizeof(old_aserver_enc),
		 new_aserver_enc, sizeof(new_aserver_enc));
	/* Don't care what port we get */
	ret = socket_sendto(so->s, m->m_data, m->m_len,&addr);
	if (ret < 0)
		return -1;

	/*
	 * Kill the socket if there's no reply in 4 minutes,
	 * but only if it's an expirable socket
	 */
	if (so->so_expire)
		so->so_expire = curtime + SO_EXPIRE;
	so->so_state = SS_ISFCONNECTED; /* So that it gets select()ed */
	return 0;
}

/*
 * XXX This should really be tcp_listen
 */
struct socket *
solisten(u_int port, u_int32_t laddr, u_int lport, int flags)
{
	SockAddress  addr;
	struct socket *so;
	int s;

	DEBUG_CALL("solisten");
	DEBUG_ARG("port = %d", port);
	DEBUG_ARG("laddr = %x", laddr);
	DEBUG_ARG("lport = %d", lport);
	DEBUG_ARG("flags = %x", flags);

	if ((so = socreate()) == NULL) {
	  /* free(so);      Not sofree() ??? free(NULL) == NOP */
	  return NULL;
	}

	/* Don't tcp_attach... we don't need so_snd nor so_rcv */
	if ((so->so_tcpcb = tcp_newtcpcb(so)) == NULL) {
		free(so);
		return NULL;
	}
	insque(so,&tcb);

	/*
	 * SS_FACCEPTONCE sockets must time out.
	 */
	if (flags & SS_FACCEPTONCE)
	   so->so_tcpcb->t_timer[TCPT_KEEP] = TCPTV_KEEP_INIT*2;

	so->so_state      = (SS_FACCEPTCONN|flags);
	sock_address_init_inet(&so->laddr, laddr, lport);
    so->so_haddr_port = port;

    if (flags & SS_IPV6)
        s = socket_loopback6_server(port, SOCKET_STREAM);
    else
        s = socket_loopback4_server(port, SOCKET_STREAM);
    if (s < 0)
        return NULL;

    socket_get_address(s, &addr);
    sock_address_init_inet(&so->faddr, alias_addr_ip, sock_address_get_port(&addr));
    so->s = s;
    return so;
}


int
sounlisten(struct socket *head, u_int  port)
{
    struct socket *so;

    for (so = head->so_next; so != head; so = so->so_next) {
        if (so->so_haddr_port == port) {
            break;
        }
    }

    if (so == head) {
        return -1;
    }

    sofcantrcvmore( so );
    sofcantsendmore( so );
    close( so->s );
    so->s = -1;
    sofree( so );
    return 0;
}


#if 0
/*
 * Data is available in so_rcv
 * Just write() the data to the socket
 * XXX not yet...
 */
static void
sorwakeup(so)
	struct socket *so;
{
/*	sowrite(so); */
/*	FD_CLR(so->s,&writefds); */
}

/*
 * Data has been freed in so_snd
 * We have room for a read() if we want to
 * For now, don't read, it'll be done in the main loop
 */
static void
sowwakeup(so)
	struct socket *so;
{
	/* Nothing, yet */
}
#endif

/*
 * Various session state calls
 * XXX Should be #define's
 * The socket state stuff needs work, these often get call 2 or 3
 * times each when only 1 was needed
 */
void
soisfconnecting(struct socket *so)
{
	so->so_state &= ~(SS_NOFDREF|SS_ISFCONNECTED|SS_FCANTRCVMORE|
			  SS_FCANTSENDMORE|SS_FWDRAIN);
	so->so_state |= SS_ISFCONNECTING; /* Clobber other states */
}

void
soisfconnected(struct socket *so)
{
	so->so_state &= ~(SS_ISFCONNECTING|SS_FWDRAIN|SS_NOFDREF);
	so->so_state |= SS_ISFCONNECTED; /* Clobber other states */
}

static void
sofcantrcvmore(struct socket *so)
{
	if ((so->so_state & SS_NOFDREF) == 0) {
		shutdown(so->s,0);
		if(global_writefds) {
		  fd_clr_ext(so->s,global_writefds);
		}
	}
	so->so_state &= ~(SS_ISFCONNECTING);
	if (so->so_state & SS_FCANTSENDMORE)
	   so->so_state = SS_NOFDREF; /* Don't select it */ /* XXX close() here as well? */
	else
	   so->so_state |= SS_FCANTRCVMORE;
}

static void
sofcantsendmore(struct socket *so)
{
	if ((so->so_state & SS_NOFDREF) == 0) {
            shutdown(so->s,1);           /* send FIN to fhost */
            if (global_readfds) {
                fd_clr_ext(so->s,global_readfds);
            }
            if (global_xfds) {
                fd_clr_ext(so->s,global_xfds);
            }
	}
	so->so_state &= ~(SS_ISFCONNECTING);
	if (so->so_state & SS_FCANTRCVMORE)
	   so->so_state = SS_NOFDREF; /* as above */
	else
	   so->so_state |= SS_FCANTSENDMORE;
}

void
soisfdisconnected(struct socket *so)
{
/*	so->so_state &= ~(SS_ISFCONNECTING|SS_ISFCONNECTED); */
/*	close(so->s); */
/*	so->so_state = SS_ISFDISCONNECTED; */
	/*
	 * XXX Do nothing ... ?
	 */
}

/*
 * Set write drain mode
 * Set CANTSENDMORE once all data has been write()n
 */
void
sofwdrain(struct socket *so)
{
	if (so->so_rcv.sb_cc)
		so->so_state |= SS_FWDRAIN;
	else
		sofcantsendmore(so);
}
