/*	$OpenBSD: if_pfsync.c,v 1.8 2003/12/15 07:11:30 mcbride Exp $	*/

/*
 * Copyright (c) 2002 Michael Shalayeff
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS RELATIVES BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF MIND, USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "bpfilter.h"
#include "pfsync.h"

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/timeout.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/bpf.h>

#ifdef	INET
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#endif

#ifdef INET6
#ifndef INET
#include <netinet/in.h>
#endif
#include <netinet6/nd6.h>
#endif /* INET6 */

#include <net/pfvar.h>
#include <net/if_pfsync.h>

#define PFSYNC_MINMTU	\
    (sizeof(struct pfsync_header) + sizeof(struct pf_state))

#ifdef PFSYNCDEBUG
#define DPRINTF(x)    do { if (pfsyncdebug) printf x ; } while (0)
int pfsyncdebug;
#else
#define DPRINTF(x)
#endif

struct pfsync_softc pfsyncif;
struct pfsyncstats pfsyncstats;

void	pfsyncattach(int);
void	pfsync_setmtu(struct pfsync_softc *, int);
int	pfsync_insert_net_state(struct pfsync_state *);
int	pfsyncoutput(struct ifnet *, struct mbuf *, struct sockaddr *,
	       struct rtentry *);
int	pfsyncioctl(struct ifnet *, u_long, caddr_t);
void	pfsyncstart(struct ifnet *);

struct mbuf *pfsync_get_mbuf(struct pfsync_softc *, u_int8_t, void **);
int	pfsync_sendout(struct pfsync_softc *sc);
void	pfsync_timeout(void *);

extern int ifqmaxlen;
extern struct timeval time;

void
pfsyncattach(int npfsync)
{
	struct ifnet *ifp;

	bzero(&pfsyncif, sizeof(pfsyncif));
	pfsyncif.sc_mbuf = NULL;
	pfsyncif.sc_mbuf_net = NULL;
	pfsyncif.sc_sp.s = NULL;
	pfsyncif.sc_sp_net.s = NULL;
	pfsyncif.sc_maxupdates = 128;
	ifp = &pfsyncif.sc_if;
	strlcpy(ifp->if_xname, "pfsync0", sizeof ifp->if_xname);
	ifp->if_softc = &pfsyncif;
	ifp->if_ioctl = pfsyncioctl;
	ifp->if_output = pfsyncoutput;
	ifp->if_start = pfsyncstart;
	ifp->if_type = IFT_PFSYNC;
	ifp->if_snd.ifq_maxlen = ifqmaxlen;
	ifp->if_hdrlen = PFSYNC_HDRLEN;
	ifp->if_baudrate = IF_Mbps(100);
	pfsync_setmtu(&pfsyncif, MCLBYTES);
	timeout_set(&pfsyncif.sc_tmo, pfsync_timeout, &pfsyncif);
	if_attach(ifp);
	if_alloc_sadl(ifp);

#if NBPFILTER > 0
	bpfattach(&pfsyncif.sc_if.if_bpf, ifp, DLT_PFSYNC, PFSYNC_HDRLEN);
#endif
}

/*
 * Start output on the pfsync interface.
 */
void
pfsyncstart(struct ifnet *ifp)
{
	struct mbuf *m;
	int s;

	for (;;) {
		s = splimp();
		IF_DROP(&ifp->if_snd);
		IF_DEQUEUE(&ifp->if_snd, m);
		splx(s);

		if (m == NULL)
			return;
		else
			m_freem(m);
	}
}

int
pfsync_insert_net_state(struct pfsync_state *sp)
{
	struct pf_state	*st = NULL;
	struct pf_rule *r = NULL;
	u_long secs;

	if (sp->creatorid == 0 && pf_status.debug >= PF_DEBUG_MISC) {
		printf("pfsync_insert_net_state: invalid creator id:"
		    "id: %016llx creatorid: %08x\n",
		    betoh64(sp->id), ntohl(sp->creatorid));
		return (EINVAL);
	}

	/*
	 * Just use the default rule until we have infrastructure to find the
	 * best matching rule.
	 */
	r = &pf_default_rule;

	if (!r->max_states || r->states < r->max_states)
		st = pool_get(&pf_state_pl, PR_NOWAIT);
	if (st == NULL)
		return (ENOMEM);
	bzero(st, sizeof(*st));

	st->rule.ptr = r;
	/* XXX get pointers to nat_rule and anchor */

	/* fill in the rest of the state entry */
	pf_state_host_ntoh(&sp->lan, &st->lan);
	pf_state_host_ntoh(&sp->gwy, &st->gwy);
	pf_state_host_ntoh(&sp->ext, &st->ext);

	pf_state_peer_ntoh(&sp->src, &st->src);
	pf_state_peer_ntoh(&sp->dst, &st->dst);

	bcopy(&sp->rt_addr, &st->rt_addr, sizeof(st->rt_addr));
	secs = time.tv_sec;
	st->creation = secs + ntohl(sp->creation);

	st->af = sp->af;
	st->proto = sp->proto;
	st->direction = sp->direction;
	st->log = sp->log;
	st->allow_opts = sp->allow_opts;

	st->id = sp->id;
	st->creatorid = sp->creatorid;
	st->sync_flags = sp->sync_flags | PFSTATE_FROMSYNC;

	secs = time.tv_sec;
	if (sp->expire)
		st->expire = 0;
	else
		st->expire = ntohl(sp->expire) + secs;

	if (pf_insert_state(st)) {
		pool_put(&pf_state_pl, st);
		return (EINVAL);
	}

	return (0);
}

void
pfsync_input(struct mbuf *m, ...)
{
	struct ip *ip = mtod(m, struct ip *);
	struct pfsync_header *ph;
	struct pfsync_softc *sc = &pfsyncif;
	struct pf_state *st, key;
	struct pfsync_state *sp;
	struct pfsync_state_upd *up;
	struct pfsync_state_del *dp;
	struct pfsync_state_clr *cp;
	struct mbuf *mp;
	int iplen, action, error, i, s, count, offp;
	u_long secs;

	pfsyncstats.pfsyncs_ipackets++;

	/* verify that we have a sync interface configured */
	if (!sc->sc_sync_ifp)
		goto done;

	/* verify that the packet came in on the right interface */
	if (sc->sc_sync_ifp != m->m_pkthdr.rcvif) {
		pfsyncstats.pfsyncs_badif++;
		goto done;
	}

	/* verify that the IP TTL is 255.  */
	if (ip->ip_ttl != PFSYNC_DFLTTL) {
		pfsyncstats.pfsyncs_badttl++;
		goto done;
	}

	iplen = ip->ip_hl << 2;

	if (m->m_pkthdr.len < iplen + sizeof(*ph)) {
		pfsyncstats.pfsyncs_hdrops++;
		goto done;
	}

	if (iplen + sizeof(*ph) > m->m_len) {
		if ((m = m_pullup(m, iplen + sizeof(*ph))) == NULL) {
			pfsyncstats.pfsyncs_hdrops++;
			goto done;
		}
		ip = mtod(m, struct ip *);
	}
	ph = (void *)ip + iplen;

	/* verify the version */
	if (ph->version != PFSYNC_VERSION) {
		pfsyncstats.pfsyncs_badver++;
		goto done;
	}

	action = ph->action;
	count = ph->count;

	/* make sure it's a valid action code */
	if (action >= PFSYNC_ACT_MAX) {
		pfsyncstats.pfsyncs_badact++;
		goto done;
	}

	switch (action) {
	case PFSYNC_ACT_CLR: {
		u_int32_t creatorid;
		if ((mp = m_pulldown(m, iplen + sizeof(*ph),
		    sizeof(*cp), &offp)) == NULL) {
			pfsyncstats.pfsyncs_badlen++;
			return;
		}

		s = splsoftnet();	
		cp = (void *)((char *)mp->m_data + iplen + PFSYNC_HDRLEN); 
		creatorid = cp->creatorid;

                RB_FOREACH(st, pf_state_tree_ext_gwy, &tree_ext_gwy) {
			if (st->creatorid == creatorid)
                        	st->timeout = PFTM_PURGE;
		}
                pf_purge_expired_states();
                splx(s);
		break;
	}
	case PFSYNC_ACT_INS:
		if ((mp = m_pulldown(m, iplen + sizeof(*ph),
		    count * sizeof(*sp), &offp)) == NULL) {
			pfsyncstats.pfsyncs_badlen++;
			return;
		}

		s = splsoftnet();	
		for (i = 0, sp = (void *)((char *)mp->m_data +
		    iplen + PFSYNC_HDRLEN); i < count; i++, sp++) {
			if ((error = pfsync_insert_net_state(sp))) {
				if (error == ENOMEM) {
					splx(s);
                       			goto done;
				}
				continue;
			}
		}
		splx(s);
		break;

	/*
	 * It's not strictly necessary for us to support the "uncompressed"
	 * update and delete actions, but it's relatively simple for us to do.
	 */
	case PFSYNC_ACT_UPD:
		if ((mp = m_pulldown(m, iplen + sizeof(*ph),
		    count * sizeof(*sp), &offp)) == NULL) {
			pfsyncstats.pfsyncs_badlen++;
			return;
		}
		
		s = splsoftnet();	
		for (i = 0, sp = (void *)((char *)mp->m_data +
		    iplen + PFSYNC_HDRLEN); i < count; i++, sp++) {
			key.id = sp->id;
			key.creatorid = sp->creatorid;

			st = pf_find_state(&key, PF_ID);
			if (st == NULL) {
				/* try to do an insert? */
				pfsyncstats.pfsyncs_badstate++;
				continue;
			}
			pf_state_peer_ntoh(&sp->src, &st->src);
			pf_state_peer_ntoh(&sp->dst, &st->dst);
			secs = time.tv_sec;
			if (sp->expire)
				st->expire = 0;
			else
				st->expire = ntohl(sp->expire) + secs;

		}
		splx(s);
		break;
	case PFSYNC_ACT_DEL:
		if ((mp = m_pulldown(m, iplen + sizeof(*ph),
		    count * sizeof(*sp), &offp)) == NULL) {
			pfsyncstats.pfsyncs_badlen++;
			return;
		}
		
		s = splsoftnet();	
		for (i = 0, sp = (void *)((char *)mp->m_data +
		    iplen + PFSYNC_HDRLEN); i < count; i++, sp++) {
			key.id = sp->id;
			key.creatorid = sp->creatorid;

			st = pf_find_state(&key, PF_ID);
			if (st == NULL) {
				pfsyncstats.pfsyncs_badstate++;
				continue;
			}
			/*
			 * XXX 
			 * pf_purge_expired_states() is expensive,
			 * we really want to purge the state directly.
			 */
			st->timeout = PFTM_PURGE;
			st->sync_flags |= PFSTATE_FROMSYNC;
		}
		pf_purge_expired_states();
		splx(s);
		break;
	case PFSYNC_ACT_UPD_C:
		if ((mp = m_pulldown(m, iplen + sizeof(*ph),
		    count * sizeof(*up), &offp)) == NULL) {
			pfsyncstats.pfsyncs_badlen++;
			return;
		}
		
		s = splsoftnet();	
		for (i = 0, up = (void *)((char *)mp->m_data +
		    iplen + PFSYNC_HDRLEN); i < count; i++, up++) {
			key.id = up->id;
			key.creatorid = up->creatorid;

			st = pf_find_state(&key, PF_ID);
			if (st == NULL) {
				/* send out a request for a full state? */
				pfsyncstats.pfsyncs_badstate++;
				continue;
			}
			pf_state_peer_ntoh(&up->src, &st->src);
			pf_state_peer_ntoh(&up->dst, &st->dst);
			secs = time.tv_sec;
			if (up->expire)
				st->expire = 0;
			else
				st->expire = ntohl(up->expire) + secs;

		}
		splx(s);
		break;
	case PFSYNC_ACT_DEL_C:
		if ((mp = m_pulldown(m, iplen + sizeof(*ph),
		    count * sizeof(*dp), &offp)) == NULL) {
			pfsyncstats.pfsyncs_badlen++;
			return;
		}
		
		s = splsoftnet();	
		for (i = 0, dp = (void *)((char *)mp->m_data +
		    iplen + PFSYNC_HDRLEN); i < count; i++, dp++) {
			key.id = dp->id;
			key.creatorid = dp->creatorid;

			st = pf_find_state(&key, PF_ID);
			if (st == NULL) {
				pfsyncstats.pfsyncs_badstate++;
				continue;
			}
			/*
			 * XXX 
			 * pf_purge_expired_states() is expensive,
			 * we really want to purge the state directly.
			 */
			st->timeout = PFTM_PURGE;
			st->sync_flags |= PFSTATE_FROMSYNC;
		}
		pf_purge_expired_states();
		splx(s);
		break;
	case PFSYNC_ACT_INS_F:
	case PFSYNC_ACT_DEL_F:
		/* not implemented */
		break;
	}
	
done:
	if (m)
		m_freem(m);
}

int
pfsyncoutput(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
	struct rtentry *rt)
{
	m_freem(m);
	return (0);
}

/* ARGSUSED */
int
pfsyncioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct proc *p = curproc;
	struct pfsync_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	struct ip_moptions *imo = &sc->sc_imo;
	struct pfsyncreq pfsyncr;
	struct ifnet    *sifp;
	int s, error;

	switch (cmd) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
	case SIOCSIFDSTADDR:
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP)
			ifp->if_flags |= IFF_RUNNING;
		else
			ifp->if_flags &= ~IFF_RUNNING;
		break;
	case SIOCSIFMTU:
		if (ifr->ifr_mtu < PFSYNC_MINMTU)
			return (EINVAL);
		if (ifr->ifr_mtu > MCLBYTES)
			ifr->ifr_mtu = MCLBYTES;
		s = splnet();
		if (ifr->ifr_mtu < ifp->if_mtu) 
			pfsync_sendout(sc);
		pfsync_setmtu(sc, ifr->ifr_mtu);
		splx(s);
		break;
	case SIOCGETPFSYNC:
		bzero(&pfsyncr, sizeof(pfsyncr));
		if (sc->sc_sync_ifp) 
			strlcpy(pfsyncr.pfsyncr_syncif,
			    sc->sc_sync_ifp->if_xname, IFNAMSIZ);
		pfsyncr.pfsyncr_maxupdates = sc->sc_maxupdates;
                if ((error = copyout(&pfsyncr, ifr->ifr_data, sizeof(pfsyncr))))
			return (error);
		break;
	case SIOCSETPFSYNC:
		if ((error = suser(p, p->p_acflag)) != 0)
			return (error);
		if ((error = copyin(ifr->ifr_data, &pfsyncr, sizeof(pfsyncr))))
			return (error);

		if (pfsyncr.pfsyncr_maxupdates > 255)
			return (EINVAL);
		sc->sc_maxupdates = pfsyncr.pfsyncr_maxupdates;
                
		if (pfsyncr.pfsyncr_syncif[0] == 0) {
			sc->sc_sync_ifp = NULL;
			break;
		}
		if ((sifp = ifunit(pfsyncr.pfsyncr_syncif)) == NULL)
			return (EINVAL);
		else if (sifp == sc->sc_sync_ifp)
			break;

		s = splnet();
		if (sifp->if_mtu < sc->sc_if.if_mtu ||
		    (sc->sc_sync_ifp != NULL && 
		    sifp->if_mtu < sc->sc_sync_ifp->if_mtu) ||
		    sifp->if_mtu < MCLBYTES - sizeof(struct ip))
			pfsync_sendout(sc);
		sc->sc_sync_ifp = sifp;
		
		pfsync_setmtu(sc, sc->sc_if.if_mtu);
		
		if (imo->imo_num_memberships > 0) {
			in_delmulti(imo->imo_membership[--imo->imo_num_memberships]);
			imo->imo_multicast_ifp = NULL;
		}

		if (sc->sc_sync_ifp) {
			struct in_addr addr;

			addr.s_addr = INADDR_PFSYNC_GROUP;
			if ((imo->imo_membership[0] =
			    in_addmulti(&addr, sc->sc_sync_ifp)) == NULL) {
				splx(s);
				return (ENOBUFS);
			}
			imo->imo_num_memberships++;
			imo->imo_multicast_ifp = sc->sc_sync_ifp;
			imo->imo_multicast_ttl = PFSYNC_DFLTTL;
			imo->imo_multicast_loop = 0;
		}
		splx(s);
		
		break;

	default:
		return (ENOTTY);
	}

	return (0);
}

void
pfsync_setmtu(struct pfsync_softc *sc, int mtu_req)
{
	int mtu;

	if (sc->sc_sync_ifp && sc->sc_sync_ifp->if_mtu < mtu_req)
		mtu = sc->sc_sync_ifp->if_mtu;
	else
		mtu = mtu_req; 

	sc->sc_maxcount = (mtu - sizeof(struct pfsync_header)) /
	    sizeof(struct pfsync_state);
	if (sc->sc_maxcount > 254)
	    sc->sc_maxcount = 254;
	sc->sc_if.if_mtu = sizeof(struct pfsync_header) +
	    sc->sc_maxcount * sizeof(struct pfsync_state);
}

struct mbuf *
pfsync_get_mbuf(struct pfsync_softc *sc, u_int8_t action, void **sp)
{
	extern int hz;
	struct pfsync_header *h;
	struct mbuf *m;
	int len;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
		sc->sc_if.if_oerrors++;
		return (NULL);
	}

	switch (action) {
	case PFSYNC_ACT_UPD_C:
		len = (sc->sc_maxcount * sizeof(struct pfsync_state_upd))
		    + sizeof(struct pfsync_header);
		break;
	case PFSYNC_ACT_DEL_C:
		len = (sc->sc_maxcount * sizeof(struct pfsync_state_del))
		    + sizeof(struct pfsync_header);
		break;
	case PFSYNC_ACT_CLR:
		len = sizeof(struct pfsync_header) +
		     sizeof(struct pfsync_state_clr);
		break;
	default:
		len = (sc->sc_maxcount * sizeof(struct pfsync_state))
		    + sizeof(struct pfsync_header);
		break;
	}

	if (len > MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if ((m->m_flags & M_EXT) == 0) {
			m_free(m);
			sc->sc_if.if_oerrors++;
			return (NULL);
		}
		m->m_data += (MCLBYTES - len) &~ (sizeof(long) - 1);
	} else 
		MH_ALIGN(m, len);

	m->m_pkthdr.rcvif = NULL;
	m->m_pkthdr.len = m->m_len = sizeof(struct pfsync_header);
	h = mtod(m, struct pfsync_header *);
	h->version = PFSYNC_VERSION;
	h->af = 0;
	h->count = 0;
	h->action = action;

	*sp = (void *)((char *)h + PFSYNC_HDRLEN);
	timeout_add(&sc->sc_tmo, hz);
	return (m);
}

int
pfsync_pack_state(u_int8_t action, struct pf_state *st)
{
	struct ifnet *ifp = &pfsyncif.sc_if;
	struct pfsync_softc *sc = ifp->if_softc;
	struct pfsync_header *h, *h_net;
	struct pfsync_state *sp = NULL;
	struct pfsync_state_upd *up = NULL;
	struct pfsync_state_del *dp = NULL;
	struct pf_rule *r;
	u_long secs;
	int s, ret = 0;
	u_int8_t i = 255, newaction = 0;

	if (action >= PFSYNC_ACT_MAX)
		return (EINVAL);

	s = splnet();
	if (sc->sc_mbuf == NULL) {
		if ((sc->sc_mbuf = pfsync_get_mbuf(sc, action,
		    (void *)&sc->sc_sp.s)) == NULL) {
			splx(s);
			return (ENOMEM);
		}
		h = mtod(sc->sc_mbuf, struct pfsync_header *);
	} else {
		h = mtod(sc->sc_mbuf, struct pfsync_header *);
		if (h->action != action) {
			pfsync_sendout(sc);
			if ((sc->sc_mbuf = pfsync_get_mbuf(sc, action,
			    (void *)&sc->sc_sp.s)) == NULL) {
				splx(s);
				return (ENOMEM);
			}
			h = mtod(sc->sc_mbuf, struct pfsync_header *);
		} else {
			/*
			 * If it's an update, look in the packet to see if
			 * we already have an update for the state.
			 */
			if (action == PFSYNC_ACT_UPD && sc->sc_maxupdates) {
				struct pfsync_state *usp =
				    (void *)((char *)h + PFSYNC_HDRLEN);

				for (i = 0; i < h->count; i++) {
					if (usp->id == st->id &&
					    usp->creatorid == st->creatorid) {
						sp = usp;
						break;
					}
					usp++;
				}
			}
		}
	}

	secs = time.tv_sec;

	if (sp == NULL) {
		/* not a "duplicate" update */
		sp = sc->sc_sp.s++;
		sc->sc_mbuf->m_pkthdr.len = 
		    sc->sc_mbuf->m_len += sizeof(struct pfsync_state);
		h->count++;
		bzero(sp, sizeof(*sp));

		sp->id = st->id;
		sp->creatorid = st->creatorid;

		pf_state_host_hton(&st->lan, &sp->lan);
		pf_state_host_hton(&st->gwy, &sp->gwy);
		pf_state_host_hton(&st->ext, &sp->ext);

		bcopy(&st->rt_addr, &sp->rt_addr, sizeof(sp->rt_addr));

		sp->creation = htonl(secs - st->creation);
		sp->packets[0] = htonl(st->packets[0]);
		sp->packets[1] = htonl(st->packets[1]);
		sp->bytes[0] = htonl(st->bytes[0]);
		sp->bytes[1] = htonl(st->bytes[1]);
		if ((r = st->rule.ptr) == NULL)
			sp->rule = htonl(-1);
		else
			sp->rule = htonl(r->nr);
		if ((r = st->anchor.ptr) == NULL)
			sp->anchor = htonl(-1);
		else
			sp->anchor = htonl(r->nr);
		sp->af = st->af;
		sp->proto = st->proto;
		sp->direction = st->direction;
		sp->log = st->log;
		sp->allow_opts = st->allow_opts;

		sp->sync_flags = st->sync_flags & PFSTATE_NOSYNC;
	} else
		sp->updates++;

	pf_state_peer_hton(&st->src, &sp->src);
	pf_state_peer_hton(&st->dst, &sp->dst);

	if (st->expire <= secs)
		sp->expire = htonl(0);
	else
		sp->expire = htonl(st->expire - secs);

	/* do we need to build "compressed" actions for network transfer? */
	if (sc->sc_sync_ifp) {
		switch (action) {
		case PFSYNC_ACT_UPD:
			newaction = PFSYNC_ACT_UPD_C;
			break;
		case PFSYNC_ACT_DEL:
			newaction = PFSYNC_ACT_DEL_C;
			break;
		default:
			/* by default we just send the uncompressed states */
			break;
		}
	}
		
	if (newaction) {
		if (sc->sc_mbuf_net == NULL) {
			if ((sc->sc_mbuf_net = pfsync_get_mbuf(sc, newaction,
			    (void *)&sc->sc_sp_net.s)) == NULL) {
				splx(s);
				return (ENOMEM);
			}
		}
		h_net = mtod(sc->sc_mbuf_net, struct pfsync_header *);
		
		switch (newaction) {
		case PFSYNC_ACT_UPD_C:
			if (i < h->count) {
				up = (void *)((char *)h_net +
				    PFSYNC_HDRLEN + (i * sizeof(*up)));
				up->updates++;
			} else {
				h_net->count++;
				sc->sc_mbuf_net->m_pkthdr.len =
				    sc->sc_mbuf_net->m_len += sizeof(*up);
				up = sc->sc_sp_net.u++;

				bzero(up, sizeof(*up));
				up->id = st->id;
				up->creatorid = st->creatorid;
			}
			up->expire = sp->expire;
			up->src = sp->src;
			up->dst = sp->dst;
			break;
		case PFSYNC_ACT_DEL_C:
			sc->sc_mbuf_net->m_pkthdr.len =
			    sc->sc_mbuf_net->m_len += sizeof(*dp);
			dp = sc->sc_sp_net.d++;
			h_net->count++;

			bzero(dp, sizeof(*dp));
			dp->id = st->id;
			dp->creatorid = st->creatorid;
			break;
		}
	}

	if (h->count == sc->sc_maxcount ||
	    (sc->sc_maxupdates && (sp->updates >= sc->sc_maxupdates)))
		ret = pfsync_sendout(sc);

	splx(s);
	return (ret);
}

int
pfsync_clear_states(u_int32_t creatorid)
{
	struct ifnet *ifp = &pfsyncif.sc_if;
	struct pfsync_softc *sc = ifp->if_softc;
	struct pfsync_state_clr *cp;
	int s, ret;

	s = splnet();
	if (sc->sc_mbuf != NULL) {
		pfsync_sendout(sc);
	}
	if ((sc->sc_mbuf = pfsync_get_mbuf(sc, PFSYNC_ACT_CLR,
	    (void *)&sc->sc_sp.c)) == NULL) {
		splx(s);
		return (ENOMEM);
	}
	sc->sc_mbuf->m_pkthdr.len = sc->sc_mbuf->m_len += sizeof(*cp);
	cp = sc->sc_sp.c;
	cp->creatorid = creatorid;

	ret = (pfsync_sendout(sc));
	splx(s);
	return (ret);
}

void
pfsync_timeout(void *v)
{
	struct pfsync_softc *sc = v;
	int s;

	s = splnet();
	pfsync_sendout(sc);
	splx(s);
}

int
pfsync_sendout(sc)
	struct pfsync_softc *sc;
{
	struct ifnet *ifp = &sc->sc_if;
	struct mbuf *m = sc->sc_mbuf;

	timeout_del(&sc->sc_tmo);
	sc->sc_mbuf = NULL;
	sc->sc_sp.s = NULL;

#if NBPFILTER > 0
	if (ifp->if_bpf)
		bpf_mtap(ifp->if_bpf, m);
#endif


	if (sc->sc_mbuf_net) {
		m_freem(m);
		m = sc->sc_mbuf_net;
		sc->sc_mbuf_net = NULL;
		sc->sc_sp_net.s = NULL;
	}

	if (sc->sc_sync_ifp) {
		struct ip *ip;
		struct ifaddr *ifa;
		struct sockaddr sa;

		M_PREPEND(m, sizeof(struct ip), M_DONTWAIT);
		if (m == NULL) {
			pfsyncstats.pfsyncs_onomem++;
			return (0);
		}
		m->m_flags |= M_MCAST;
		ip = mtod(m, struct ip *);
		ip->ip_v = IPVERSION;
		ip->ip_hl = sizeof(*ip) >> 2;
		ip->ip_tos = IPTOS_LOWDELAY;
		ip->ip_len = htons(m->m_pkthdr.len);
		ip->ip_id = htons(ip_randomid());
		ip->ip_off = htons(IP_DF);
		ip->ip_ttl = PFSYNC_DFLTTL;
		ip->ip_p = IPPROTO_PFSYNC;
		ip->ip_sum = 0;

		bzero(&sa, sizeof(sa));
		sa.sa_family = AF_INET;
		ifa = ifaof_ifpforaddr(&sa, sc->sc_sync_ifp);
		if (ifa == NULL)
			return (0);
		ip->ip_src.s_addr = ifatoia(ifa)->ia_addr.sin_addr.s_addr;
		ip->ip_dst.s_addr = INADDR_PFSYNC_GROUP;

		pfsyncstats.pfsyncs_opackets++;

		if (ip_output(m, NULL, NULL, IP_RAWOUTPUT, &sc->sc_imo, NULL))
			pfsyncstats.pfsyncs_oerrors++;
	} else {
		m_freem(m);
	}

	return (0);
}
