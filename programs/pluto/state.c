/* routines for state objects
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2001, 2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009,2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2013 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012 Wes Hardaker <opensource@hardakers.net>
 * Copyright (C) 2012 Bram <bram-bcrafjna-erqzvar@spam.wizbit.be>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2013 Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <libreswan.h>

#include "sysdep.h"
#include "constants.h"
#include "defs.h"
#include "id.h"
#include "x509.h"
#include "certs.h"
#ifdef XAUTH_HAVE_PAM
#include <security/pam_appl.h>
#include "ikev1_xauth.h"	/* just for state_deletion_xauth_cleanup() */
#endif
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "ikev1_msgid.h"
#include "kernel.h"             /* needs connections.h */
#include "log.h"
#include "packet.h"             /* so we can calculate sizeof(struct isakmp_hdr) */
#include "keys.h"               /* for free_public_key */
#include "rnd.h"
#include "timer.h"
#include "whack.h"
#include "demux.h"      /* needs packet.h */
#include "pending.h"
#include "ipsec_doi.h"  /* needs demux.h and state.h */

#include "sha1.h"
#include "md5.h"
#include "cookie.h"
#include "crypto.h" /* requires sha1.h and md5.h */
#include "spdb.h"

#include <nss.h>
#include <pk11pub.h>
#include <keyhi.h>

/*
 * Global variables: had to go somewhere, might as well be this file.
 */

u_int16_t pluto_port = IKE_UDP_PORT;                    /* Pluto's port */
u_int16_t pluto_nat_port = NAT_IKE_UDP_PORT; /* Pluto's NAT-T port */

/*
 * This file has the functions that handle the
 * state hash table and the Message ID list.
 */

/* humanize_number: make large numbers clearer by expressing them as KB or MB, as appropriate.
 * The prefix is literally copied into the output.
 * Tricky representation: if the prefix starts with !, the number
 * is taken as kilobytes.  Thus the caller does not scaling, with the attendant
 * risk of overflow.  The ! is not printed.
 */
static char *humanize_number(unsigned long num,
			     char *buf,
			     const char *buf_roof,
			     const char *prefix)
{
	size_t buf_len = buf_roof - buf;
	unsigned long to_print = num;
	const char *suffix;
	int ret;
	bool kilos = prefix[0] == '!';

	if (!kilos && num < 1024) {
		suffix = "B";
	} else {
		if (!kilos)
			to_print /= 1024;

		if (to_print < 1024) {
			suffix = "KB";
		} else {
			to_print /= 1024;
			suffix = "MB";
		}
	}

	ret = snprintf(buf, buf_len, "%s%lu%s", prefix, to_print,
		       suffix + kilos);
	if (ret < 0 || (size_t) ret >= buf_len)
		return buf;

	return buf + ret;
}


/* State Table Functions
 *
 * The statetable is organized as a hash table.
 * The hash is purely based on the icookie and rcookie.
 * Each has chain is a doubly linked list.
 *
 * The phase 1 initiator does does not at first know the
 * responder's cookie, so the state will have to be rehashed
 * when that becomes known.
 *
 * In IKEv2, cookies are renamed IKE SA SPIs.
 *
 * In IKEv2, all children have the same cookies as their parent.
 * This means that you can look along that single chain for
 * your relatives.
 */

#define STATE_TABLE_SIZE 32

static struct state *statetable[STATE_TABLE_SIZE];

static struct state **state_hash(const u_char *icookie, const u_char *rcookie)
{
	u_int i = 0, j;

	DBG(DBG_RAW | DBG_CONTROL, {
		    DBG_dump("ICOOKIE:", icookie, COOKIE_SIZE);
		    DBG_dump("RCOOKIE:", rcookie, COOKIE_SIZE);
	    });

	/* XXX the following hash is pretty pathetic */

	for (j = 0; j < COOKIE_SIZE; j++)
		i = i * 407 + icookie[j] + rcookie[j];

	i = i % STATE_TABLE_SIZE;

	DBG(DBG_CONTROL, DBG_log("state hash entry %d", i));

	return &statetable[i];
}

/* Get a state object.
 * Caller must schedule an event for this object so that it doesn't leak.
 * Caller must insert_state().
 */
struct state *new_state(void)
{
	static const struct state blank_state;  /* initialized all to zero & NULL */
	static so_serial_t next_so = SOS_FIRST;
	struct state *st;

	st = clone_thing(blank_state, "struct state in new_state()");
	st->st_serialno = next_so++;
	passert(next_so > SOS_FIRST);   /* overflow can't happen! */
	st->st_whack_sock = NULL_FD;

	anyaddr(AF_INET, &st->hidden_variables.st_nat_oa);
	anyaddr(AF_INET, &st->hidden_variables.st_natd);

	DBG(DBG_CONTROL, DBG_log("creating state object #%lu at %p",
				 st->st_serialno, (void *) st));
	return st;
}

/*
 * Initialize the state table
 */
void init_states(void)
{
	int i;

	for (i = 0; i < STATE_TABLE_SIZE; i++)
		statetable[i] = (struct state *) NULL;
}

void v1_delete_state_by_xauth_name(struct state *st, void *name)
{
	/* only support deleting ikev1 with xauth user name */
	if (st->st_ikev2)
		return;

	if (IS_IKE_SA(st) && streq(st->st_xauth_username, name)) {
		delete_my_family(st, FALSE);
		/* note: no md->st to clear */
	}
}

/* Find the state object with this serial number.
 * This allows state object references that don't turn into dangerous
 * dangling pointers: reference a state by its serial number.
 * Returns NULL if there is no such state.
 * If this turns out to be a significant CPU hog, it could be
 * improved to use a hash table rather than sequential seartch.
 */
struct state *state_with_serialno(so_serial_t sn)
{
	if (sn >= SOS_FIRST) {
		struct state *st;
		int i;

		for (i = 0; i < STATE_TABLE_SIZE; i++)
			for (st = statetable[i]; st != NULL;
			     st = st->st_hashchain_next)
				if (st->st_serialno == sn)
					return st;
	}
	return NULL;
}

/* Insert a state object in the hash table. The object is inserted
 * at the begining of list.
 * Needs cookies, connection, and msgid.
 */
void insert_state(struct state *st)
{
	struct state **p = state_hash(st->st_icookie, st->st_rcookie);

	passert(st->st_hashchain_prev == NULL &&
		st->st_hashchain_next == NULL);

	DBG(DBG_CONTROL,
	    DBG_log("inserting state object #%lu",
		    st->st_serialno));

	if (*p != NULL) {
		/* hash chain is not empty; stick st at front */
		passert((*p)->st_hashchain_prev == NULL);
		(*p)->st_hashchain_prev = st;
	}
	st->st_hashchain_next = *p;
	*p = st;

	/* Ensure that somebody is in charge of killing this state:
	 * if no event is scheduled for it, schedule one to discard the state.
	 * If nothing goes wrong, this event will be replaced by
	 * a more appropriate one.
	 */
	if (st->st_event == NULL)
		event_schedule(EVENT_SO_DISCARD, 0, st);

	refresh_state(st);
}

/* common code to unhash a state: shared by rehash_state and unhash_state
 * The difference is in the rcookie.
 */
static void unhash_state_common(struct state *st, bool rcookie_may_be_previously_known)
{
	struct state **p;

	/* unlink from forward chain (might change bucket head) */

	if (st->st_hashchain_prev != NULL) {
		p = &st->st_hashchain_prev->st_hashchain_next;
	} else if (rcookie_may_be_previously_known) {
		p = state_hash(st->st_icookie, st->st_rcookie);
		if (*p != st)
			p = state_hash(st->st_icookie, zero_cookie);
	} else {
		p = state_hash(st->st_icookie, zero_cookie);
	}

	passert(*p == st);
	*p = st->st_hashchain_next;

	/* unlink from backward chain */
	if (st->st_hashchain_next != NULL) {
		passert(st->st_hashchain_next->st_hashchain_prev == st);
		st->st_hashchain_next->st_hashchain_prev =
			st->st_hashchain_prev;
	}

	st->st_hashchain_next = st->st_hashchain_prev = NULL;
}

/*
 * unlink a state object from the hash table that had a zero
 * rcookie before, and rehash it into the right place
 */
void rehash_state(struct state *st)
{
	/* unlink from forward chain */

	DBG(DBG_CONTROL,
	    DBG_log("rehashing state object #%lu",
		    st->st_serialno));

	unhash_state_common(st, FALSE);

	/* now, re-insert */
	insert_state(st);
}

/* unlink a state object from the hash table, but don't free it
 */
void unhash_state(struct state *st)
{
	unhash_state_common(st, TRUE);
}

/* Free the Whack socket file descriptor.
 * This has the side effect of telling Whack that we're done.
 */
void release_whack(struct state *st)
{
	close_any(st->st_whack_sock);
}

void release_fragments(struct state *st)
{
	struct ike_frag *frag = st->ike_frags;

	while (frag != NULL) {
		struct ike_frag *this = frag;

		frag = this->next;
		release_md(this->md);

		pfree(this);
	}

	st->ike_frags = NULL;
}

/* delete a state object */
void delete_state(struct state *st)
{
	struct connection *const c = st->st_connection;
	struct state *old_cur_state = cur_state == st ? NULL : cur_state;

	libreswan_log("deleting state #%lu (%s)", st->st_serialno,
				enum_show(&state_names, st->st_state));

#ifdef USE_LINUX_AUDIT
	/* only log parent state deletes, we log children in ipsec_delete_sa() */
	if (IS_IKE_SA_ESTABLISHED(st) || st->st_state == STATE_IKESA_DEL)
		linux_audit_conn(st, LAK_PARENT_DESTROY);
#endif

	if (IS_IPSEC_SA_ESTABLISHED(st->st_state)) {
		/* Note that a state/SA can have more then one of ESP/AH/IPCOMP */
		if (st->st_esp.present) {
			char statebuf[1024];
			char *sbcp = humanize_number(st->st_esp.peer_bytes,
					       statebuf,
					       statebuf + sizeof(statebuf),
					       "ESP traffic information: in=");

			(void)humanize_number(st->st_esp.our_bytes,
					       sbcp,
					       statebuf + sizeof(statebuf),
					       " out=");
			loglog(RC_INFORMATIONAL, "%s%s%s",
				statebuf,
				(st->st_xauth_username[0] != '\0') ? " XAUTHuser=" : "",
				st->st_xauth_username);
		}

		if (st->st_ah.present) {
			char statebuf[1024];
			char *sbcp = humanize_number(st->st_ah.peer_bytes,
					       statebuf,
					       statebuf + sizeof(statebuf),
					       "AH traffic information: in=");

			(void)humanize_number(st->st_ah.our_bytes,
					       sbcp,
					       statebuf + sizeof(statebuf),
					       " out=");
			loglog(RC_INFORMATIONAL, "%s%s%s",
				statebuf,
				(st->st_xauth_username[0] != '\0') ? " XAUTHuser=" : "",
				st->st_xauth_username);
		}

		if (st->st_ipcomp.present) {
			char statebuf[1024];
			char *sbcp = humanize_number(st->st_ipcomp.peer_bytes,
					       statebuf,
					       statebuf + sizeof(statebuf),
					       " IPCOMP traffic information: in=");

			(void)humanize_number(st->st_ipcomp.our_bytes,
					       sbcp,
					       statebuf + sizeof(statebuf),
					       " out=");
			loglog(RC_INFORMATIONAL, "%s%s%s",
				statebuf,
				(st->st_xauth_username[0] != '\0') ? " XAUTHuser=" : "",
				st->st_xauth_username);
		}
	}

#ifdef XAUTH_HAVE_PAM
	state_deletion_xauth_cleanup(st);
#endif

	/* If DPD is enabled on this state object, clear any pending events */
	if (st->st_dpd_event != NULL)
		delete_dpd_event(st);

	/* clear any ikev2 liveness events */
	if (st->st_ikev2)
		delete_liveness_event(st);

	if (st->st_rel_whack_event != NULL) {
		pfreeany(st->st_rel_whack_event);
		st->st_rel_whack_event = NULL;
	}

	/* if there is a suspended state transition, disconnect us */
	if (st->st_suspended_md != NULL) {
		passert(st->st_suspended_md->st == st);
		DBG(DBG_CONTROL, DBG_log("disconnecting state #%lu from md",
					 st->st_serialno));
		st->st_suspended_md->st = NULL;
	}

	/* tell the other side of any IPSEC SAs that are going down */
	if (IS_IPSEC_SA_ESTABLISHED(st->st_state) ||
			IS_ISAKMP_SA_ESTABLISHED(st->st_state)) {
		if (IS_CHILD_SA(st) &&
		    state_with_serialno(st->st_clonedfrom) == NULL) {
			/* ??? in v2, there must be a parent */
			DBG(DBG_CONTROL, DBG_log("deleting state but IKE SA does not exist for this child SA so Informational Exchange cannot be sent"));
			change_state(st, STATE_CHILDSA_DEL);
		} else  {
			/*
			 * ??? in IKE v2, we should not immediately delete:
			 * we should use an Informational Exchange to co-ordinate deletion.
			 * ikev2_delete_out doesn't really accomplish this.
			 */
			send_delete(st);
		}
	}

	delete_event(st); /* delete any pending timer event */

	/* Ditch anything pending on ISAKMP SA being established.
	 * Note: this must be done before the unhash_state to prevent
	 * flush_pending_by_state inadvertently and prematurely
	 * deleting our connection.
	 */
	flush_pending_by_state(st);

	/*
	 * if there is anything in the cryptographic queue, then remove this
	 * state from it.
	 */
	delete_cryptographic_continuation(st);

	/* effectively, this deletes any ISAKMP SA that this state represents */
	unhash_state(st);

	/*
	 * tell kernel to delete any IPSEC SA
	 * ??? we ought to tell peer to delete IPSEC SAs
	 */
	if (IS_IPSEC_SA_ESTABLISHED(st->st_state) ||
	    (IS_CHILD_SA_ESTABLISHED(st) || st->st_state == STATE_CHILDSA_DEL))
		delete_ipsec_sa(st, FALSE);
	else if (IS_ONLY_INBOUND_IPSEC_SA_ESTABLISHED(st->st_state))
		delete_ipsec_sa(st, TRUE);

	if (c->newest_ipsec_sa == st->st_serialno)
		c->newest_ipsec_sa = SOS_NOBODY;

	if (c->newest_isakmp_sa == st->st_serialno)
		c->newest_isakmp_sa = SOS_NOBODY;

	/*
	 * fake a state change here while we are still associated with a
	 * connection.  Without this the state logging (when enabled) cannot
	 * work out what happened.
	 */
	fake_state(st, STATE_UNDEFINED);

	st->st_connection = NULL;       /* we might be about to free it */
	cur_state = old_cur_state;      /* without st_connection, st isn't complete */
	connection_discard(c);

	change_state(st, STATE_UNDEFINED);

	release_whack(st);

	/* from here on we are just freeing RAM */

	ikev1_clear_msgid_list(st);
	unreference_key(&st->st_peer_pubkey);
	release_fragments(st);

	free_sa(st->st_sadb);
	st->st_sadb = NULL;

	clear_dh_from_state(st);

	freeanychunk(st->st_firstpacket_me);
	freeanychunk(st->st_firstpacket_him);
	freeanychunk(st->st_tpacket);
	freeanychunk(st->st_rpacket);
	freeanychunk(st->st_p1isa);
	freeanychunk(st->st_gi);
	freeanychunk(st->st_gr);
	freeanychunk(st->st_ni);
	freeanychunk(st->st_nr);


#    define free_any_nss_symkey(p)  { \
		if ((p) != NULL) { \
			PK11_FreeSymKey(p); \
			(p) = NULL; \
		} \
	}
	/* ??? free_any_nss_symkey(st->st_shared_nss); */
	free_any_nss_symkey(st->st_skeyseed_nss);	/* same as st_skeyid_nss */
	free_any_nss_symkey(st->st_skey_d_nss);	/* same as st_skeyid_d_nss */
	free_any_nss_symkey(st->st_skey_ai_nss);	/* same as st_skeyid_a_nss */
	free_any_nss_symkey(st->st_skey_ar_nss);
	free_any_nss_symkey(st->st_skey_ei_nss);	/* same as st_skeyid_e_nss */
	free_any_nss_symkey(st->st_skey_er_nss);
	free_any_nss_symkey(st->st_skey_pi_nss);
	free_any_nss_symkey(st->st_skey_pr_nss);
	free_any_nss_symkey(st->st_enc_key_nss);
#   undef free_any_nss_symkey

#   define wipe_any(p, l) { \
		if ((p) != NULL) { \
			memset((p), 0x00, (l)); \
			pfree(p); \
			(p) = NULL; \
		} \
	}
	wipe_any(st->st_ah.our_keymat, st->st_ah.keymat_len);
	wipe_any(st->st_ah.peer_keymat, st->st_ah.keymat_len);
	wipe_any(st->st_esp.our_keymat, st->st_esp.keymat_len);
	wipe_any(st->st_esp.peer_keymat, st->st_esp.keymat_len);

	wipe_any(st->st_xauth_password.ptr, st->st_xauth_password.len);
#   undef wipe_any

#ifdef HAVE_LABELED_IPSEC
	pfreeany(st->sec_ctx);
#endif
	zero(st);
	pfree(st);
}

/*
 * Is a connection in use by some state?
 */
bool states_use_connection(const struct connection *c)
{
	/* are there any states still using it? */
	struct state *st = NULL;
	int i;

	for (i = 0; st == NULL && i < STATE_TABLE_SIZE; i++)
		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next)
			if (st->st_connection == c)
				return TRUE;

	return FALSE;
}

/* delete all states that were created for a given connection,
 * additionally delete any states for which func(st, c)
 * returns true.
 */
static void foreach_states_by_connection_func_delete(struct connection *c,
					      bool (*comparefunc)(
						      struct state *st,
						      struct connection *c))
{
	int pass;

	/* this kludge avoids an n^2 algorithm */

	/* We take two passes so that we delete any ISAKMP SAs last.
	 * This allows Delete Notifications to be sent.
	 * ?? We could probably double the performance by caching any
	 * ISAKMP SA states found in the first pass, avoiding a second.
	 */
	for (pass = 0; pass != 2; pass++) {
		int i;

		/* For each hash chain... */
		for (i = 0; i < STATE_TABLE_SIZE; i++) {
			struct state *st;

			/* For each state in the hash chain... */
			for (st = statetable[i]; st != NULL; ) {
				struct state *this = st;

				st = st->st_hashchain_next; /* before this is deleted */

				/* on pass 2, ignore phase2 states */
				if (pass == 1 &&
				    IS_ISAKMP_SA_ESTABLISHED(this->st_state))
					continue;

				/* call comparison function */
				if ((*comparefunc)(this, c)) {
					struct state *old_cur_state =
						cur_state == this ?
						  NULL : cur_state;
					lset_t old_cur_debugging =
						cur_debugging;

					set_cur_state(this);

					DBG(DBG_CONTROL, {
						char cib[CONN_INST_BUF];
						DBG_log("deleting state #%lu (%s) \"%s\"%s",
							this->st_serialno,
							enum_show(&state_names,
								this->st_state),
							c->name,
							fmt_conn_instance(c, cib));
					});

					delete_state(this);
					/* note: no md->st to clear */

					cur_state = old_cur_state;
					set_debugging(old_cur_debugging);
				}
			}
		}
	}
}

/*
 * delete all states that were created for a given connection.
 * if relations == TRUE, then also delete states that share
 * the same phase 1 SA.
 */
static bool same_phase1_sa_relations(struct state *this,
				     struct connection *c)
{
	so_serial_t parent_sa = c->newest_isakmp_sa;

	return this->st_connection == c ||
	       (parent_sa != SOS_NOBODY &&
		this->st_clonedfrom == parent_sa);
}

/*
 * Delete all states that have somehow not ben deleted yet
 * but using interfaces that are going down
 */

void delete_states_dead_interfaces(void)
{
	struct state *st = NULL;
	int i;

	for (i = 0; st == NULL && i < STATE_TABLE_SIZE; i++)
		for (st = statetable[i]; st != NULL; ) {
			struct state *this = st;

			st = st->st_hashchain_next; /* before this is deleted */
			if (this->st_interface &&
			    this->st_interface->change == IFN_DELETE) {
				libreswan_log(
					"deleting lasting state #%lu on interface (%s) which is shutting down",
					this->st_serialno,
					this->st_interface->ip_dev->id_vname);
				delete_state(this);
				/* note: no md->st to clear */
			}
		}
}

/*
 * delete all states that were created for a given connection.
 * if relations == TRUE, then also delete states that share
 * the same phase 1 SA.
 */
static bool same_phase1_sa(struct state *this,
			   struct connection *c)
{
	return this->st_connection == c;
}

void delete_states_by_connection(struct connection *c, bool relations)
{
	enum connection_kind ck = c->kind;
	struct spd_route *sr;

	/* save this connection's isakmp SA,
	 * since it will get set to later SOS_NOBODY
	 */
	if (ck == CK_INSTANCE)
		c->kind = CK_GOING_AWAY;

	foreach_states_by_connection_func_delete(c,
		relations ? same_phase1_sa_relations : same_phase1_sa);

	/*
	 * Seems to dump here because 1 of the states is NULL.  Removing the Assert
	 * makes things work.  We should fix this eventually.
	 *
	 *  passert(c->newest_ipsec_sa == SOS_NOBODY
	 *  && c->newest_isakmp_sa == SOS_NOBODY);
	 *
	 */

	sr = &c->spd;
	while (sr != NULL) {
		passert(sr->eroute_owner == SOS_NOBODY);
		passert(sr->routing != RT_ROUTED_TUNNEL);
		sr = sr->next;
	}

	if (ck == CK_INSTANCE) {
		c->kind = ck;
		delete_connection(c, relations);
	}
}

/*
 * delete_p2states_by_connection - deletes only the phase 2 of conn
 *
 * @c - the connection whose states need to be removed.
 *
 * This is like delete_states_by_connection with relations=TRUE,
 * but it only deletes phase 2 states.
 */
static bool same_phase1_no_phase2(struct state *this,
				  struct connection *c)
{
	if (IS_ISAKMP_SA_ESTABLISHED(this->st_state))
		return FALSE;
	else
		return same_phase1_sa_relations(this, c);
}

void delete_p2states_by_connection(struct connection *c)
{
	enum connection_kind ck = c->kind;

	/* save this connection's isakmp SA,
	 * since it will get set to later SOS_NOBODY
	 */
	if (ck == CK_INSTANCE)
		c->kind = CK_GOING_AWAY;

	foreach_states_by_connection_func_delete(c, same_phase1_no_phase2);

	if (ck == CK_INSTANCE) {
		c->kind = ck;
		delete_connection(c, TRUE);
	}
}

/* Walk through the state table, and delete each state whose phase 1 (IKE)
 * peer is among those given.
 * TODO: This function is only called for ipsec whack --crash peer, but
 * it currently does not work for IKEv2, since IS_PHASE1() only works on IKEv1
 * Filed as bug http://bugs.xelerance.com/view.php?id=971
 */
void delete_states_by_peer(const ip_address *peer)
{
	char peerstr[ADDRTOT_BUF];
	int i, ph1;

	addrtot(peer, 0, peerstr, sizeof(peerstr));

	whack_log(RC_COMMENT, "restarting peer %s\n", peerstr);

	/* first restart the phase1s */
	for (ph1 = 0; ph1 < 2; ph1++) {
		/* For each hash chain... */
		for (i = 0; i < STATE_TABLE_SIZE; i++) {
			struct state *st;

			/* For each state in the hash chain... */
			for (st = statetable[i]; st != NULL; ) {
				struct state *this = st;
				struct connection *c = this->st_connection;

				st = st->st_hashchain_next; /* before this is deleted */

				DBG(DBG_CONTROL, {
					ipstr_buf b;
					DBG_log("comparing %s to %s",
						ipstr(&this->st_remoteaddr, &b),
						peerstr);
				});

				if (sameaddr(&this->st_remoteaddr, peer)) {
					if (ph1 == 0 &&
					    IS_IKE_SA(this)) {
						whack_log(RC_COMMENT,
							  "peer %s for connection %s crashed, replacing",
							  peerstr,
							  c->name);
						ipsecdoi_replace(this, LEMPTY,
								 LEMPTY, 1);
					} else {
						delete_event(this);
						event_schedule(
							EVENT_SA_REPLACE, 0,
							this);
					}
				}
			}
		}
	}
}

/*
 * IKEv1: Duplicate a Phase 1 state object, to create a Phase 2 object.
 * IKEv2: Duplicate a Parent SA state object, to create a Child SA object
 *
 * Caller must schedule an event for this object so that it doesn't leak.
 * Caller must insert_state().
 */
struct state *duplicate_state(struct state *st)
{
	struct state *nst;

	DBG(DBG_CONTROL, DBG_log("duplicating state object #%lu",
				 st->st_serialno));

	/* record use of the Phase 1 / Parent state */
	st->st_outbound_count++;
	st->st_outbound_time = mononow();

	nst = new_state();

	memcpy(nst->st_icookie, st->st_icookie, COOKIE_SIZE);
	memcpy(nst->st_rcookie, st->st_rcookie, COOKIE_SIZE);
	nst->st_connection = st->st_connection;

	nst->quirks = st->quirks;
	nst->hidden_variables = st->hidden_variables;
	nst->st_remoteaddr = st->st_remoteaddr;
	nst->st_remoteport = st->st_remoteport;
	nst->st_localaddr = st->st_localaddr;
	nst->st_localport = st->st_localport;
	nst->st_interface = st->st_interface;
	nst->st_clonedfrom = st->st_serialno;
	nst->st_import = st->st_import;
	nst->st_ikev2 = st->st_ikev2;
	nst->st_seen_fragvid = st->st_seen_fragvid;
	nst->st_seen_fragments = st->st_seen_fragments;
	nst->st_event = NULL;

#   define clone_nss_symkey_field(field) { \
		nst->field = st->field; \
		if (nst->field != NULL) \
			PK11_ReferenceSymKey(nst->field); \
	}
	clone_nss_symkey_field(st_skeyseed_nss);	/* same as st_skeyid_nss */
	clone_nss_symkey_field(st_skey_d_nss);	/* same as st_skeyid_d_nss */
	clone_nss_symkey_field(st_skey_ai_nss);	/* same as st_skeyid_a_nss */
	clone_nss_symkey_field(st_skey_ar_nss);
	clone_nss_symkey_field(st_skey_ei_nss);	/* same as st_skeyid_e_nss */
	clone_nss_symkey_field(st_skey_er_nss);
	clone_nss_symkey_field(st_skey_pi_nss);
	clone_nss_symkey_field(st_skey_pr_nss);
	clone_nss_symkey_field(st_enc_key_nss);
#   undef clone_nss_symkey_field

	/* v2 duplication of state */
#   define clone_chunk(ch, name) \
	clonetochunk(nst->ch, st->ch.ptr, st->ch.len, name)

	clone_chunk(st_ni, "st_ni in duplicate_state");
	clone_chunk(st_nr, "st_nr in duplicate_state");
#   undef clone_chunk

	nst->st_oakley = st->st_oakley;

	jam_str(nst->st_xauth_username, sizeof(nst->st_xauth_username),
		st->st_xauth_username);

	return nst;
}

void for_each_state(void (*f)(struct state *, void *data), void *data)
{
	struct state *ocs = cur_state;
	int i;

	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		struct state *st;

		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next) {
			set_cur_state(st);
			f(st, data);
		}
	}
	cur_state = ocs;
}

/*
 * Find a state object for an IKEv1 state
 */
struct state *find_state_ikev1(const u_char *icookie,
			       const u_char *rcookie,
			       msgid_t /*network order*/ msgid)
{
	struct state *st = *state_hash(icookie, rcookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    memeq(rcookie, st->st_rcookie, COOKIE_SIZE) &&
		    !st->st_ikev2) {
			DBG(DBG_CONTROL,
			    DBG_log("v1 peer and cookies match on #%ld, provided msgid %08lx vs %08lx",
				    st->st_serialno,
				    (long unsigned)ntohl(msgid),
				    (long unsigned)ntohl(st->st_msgid)));
			if (msgid == st->st_msgid)
				break;
		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL) {
			    DBG_log("v1 state object not found");
		    } else {
			    DBG_log("v1 state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
		    }
	    });

	return st;
}

#ifdef HAVE_LABELED_IPSEC
struct state *find_state_ikev1_loopback(const u_char *icookie,
					const u_char *rcookie,
					msgid_t /*network order*/ msgid,
					const struct msg_digest *md)
{
	struct state *st = *state_hash(icookie, rcookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    memeq(rcookie, st->st_rcookie, COOKIE_SIZE) &&
		    !st->st_ikev2) {
			DBG(DBG_CONTROL,
			    DBG_log("loopback: v1 peer and cookies match on #%ld, provided msgid %08lx vs %08lx",
				    st->st_serialno,
				    (long unsigned)ntohl(msgid),
				    (long unsigned)ntohl(st->st_msgid)));
			if (msgid == st->st_msgid &&
			    !(st->st_tpacket.ptr &&
			      memeq(st->st_tpacket.ptr, md->packet_pbs.start,
				     pbs_room(&md->packet_pbs))))
				break;
		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL)
			    DBG_log("loopback: v1 state object not found");
		    else
			    DBG_log("loopback: v1 state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
	    });

	return st;
}
#endif

/*
 * Find a state object for an IKEv2 state.
 * Note: only finds parent states.
 */
struct state *find_state_ikev2_parent(const u_char *icookie,
				      const u_char *rcookie)
{
	struct state *st = *state_hash(icookie, rcookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    memeq(rcookie, st->st_rcookie, COOKIE_SIZE) &&
		    st->st_ikev2 &&
		    !IS_CHILD_SA(st)) {
			DBG(DBG_CONTROL,
			    DBG_log("parent v2 peer and cookies match on #%ld",
				    st->st_serialno));
			break;
		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL) {
			    DBG_log("parent v2 state object not found");
		    } else {
			    DBG_log("v2 state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
		    }
	    });

	return st;
}

/*
 * Find a state object for an IKEv2 state, looking by icookie only.
 * Note: only finds parent states.
 */
struct state *find_state_ikev2_parent_init(const u_char *icookie)
{
	struct state *st = *state_hash(icookie, zero_cookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    st->st_ikev2 &&
		    !IS_CHILD_SA(st)) {
			DBG(DBG_CONTROL,
			    DBG_log("parent_init v2 peer and cookies match on #%ld",
				    st->st_serialno));
			break;
		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL) {
			    DBG_log("parent_init v2 state object not found");
		    } else {
			    DBG_log("v2 state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
		    }
	    });

	return st;
}

/*
 * Find a state object for an IKEv2 state, a response that includes a msgid.
 */
struct state *find_state_ikev2_child(const u_char *icookie,
				     const u_char *rcookie,
				     msgid_t msgid)
{
	struct state *st = *state_hash(icookie, rcookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    memeq(rcookie, st->st_rcookie, COOKIE_SIZE) &&
		    st->st_ikev2 &&
		    st->st_msgid == msgid) {
			DBG(DBG_CONTROL,
			    DBG_log("v2 peer, cookies and msgid match on #%ld",
				    st->st_serialno));
			break;
		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL) {
			    DBG_log("v2 state object not found");
		    } else {
			    DBG_log("v2 state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
		    }
	    });

	return st;
}

/*
 * Find a state object for an IKEv2 child state to delete.
 * In IKEv2, child states can only be distingusihed based on protocols and SPIs
 */
struct state *find_state_ikev2_child_to_delete(const u_char *icookie,
					       const u_char *rcookie,
					       u_int8_t protoid,
					       ipsec_spi_t spi)
{
	struct state *st = *state_hash(icookie, rcookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    memeq(rcookie, st->st_rcookie, COOKIE_SIZE) &&
		    st->st_ikev2 && IS_CHILD_SA(st)) {
			struct ipsec_proto_info *pr;

			switch (protoid) {
			case PROTO_IPSEC_AH:
				pr = &st->st_ah;
				break;
			case PROTO_IPSEC_ESP:
				pr = &st->st_esp;
				break;
			default:
				bad_case(protoid);
			}

			if (pr->present) {
				if (pr->attrs.spi == spi)
					break;
				if (pr->our_spi == spi)
					break;
			}

		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL) {
			    DBG_log("v2 child state object not found");
		    } else {
			    DBG_log("v2 child state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
		    }
	    });

	return st;
}

/*
 * Find a state object.
 */
struct state *ikev1_find_info_state(const u_char *icookie,
			      const u_char *rcookie,
			      const ip_address *peer UNUSED,
			      msgid_t /*network order*/ msgid)
{
	struct state *st = *state_hash(icookie, rcookie);

	while (st != (struct state *) NULL) {
		if (memeq(icookie, st->st_icookie, COOKIE_SIZE) &&
		    memeq(rcookie, st->st_rcookie, COOKIE_SIZE)) {
			DBG(DBG_CONTROL,
			    DBG_log("peer and cookies match on #%ld, provided msgid %08lx vs %08lx/%08lx",
				    st->st_serialno,
				    (long unsigned)ntohl(msgid),
				    (long unsigned)ntohl(st->st_msgid),
				    (long unsigned)ntohl(st->st_msgid_phase15)));
			if ((st->st_msgid_phase15 != v1_MAINMODE_MSGID &&
			     msgid == st->st_msgid_phase15) ||
			    msgid == st->st_msgid)
				break;
		}
		st = st->st_hashchain_next;
	}

	DBG(DBG_CONTROL, {
		    if (st == NULL) {
			    DBG_log("p15 state object not found");
		    } else {
			    DBG_log("p15 state object #%lu found, in %s",
				    st->st_serialno,
				    enum_show(&state_names, st->st_state));
		    }
	    });

	return st;
}

/* Find the state that sent a packet
 * ??? this could be expensive -- it should be rate-limited to avoid DoS
 */
struct state *find_sender(size_t packet_len, u_char *packet)
{
	int i;
	struct state *st;

	if (packet_len >= sizeof(struct isakmp_hdr)) {
		for (i = 0; i < STATE_TABLE_SIZE; i++)
			for (st = statetable[i]; st != NULL;
			     st = st->st_hashchain_next)
				if (st->st_tpacket.ptr != NULL &&
				    st->st_tpacket.len == packet_len &&
				    memeq(st->st_tpacket.ptr, packet,
					   packet_len))
					return st;
	}

	return NULL;
}

struct state *find_phase2_state_to_delete(const struct state *p1st,
					  u_int8_t protoid,
					  ipsec_spi_t spi,
					  bool *bogus)
{
	struct state *st;
	int i;

	*bogus = FALSE;
	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next) {
			if (IS_IPSEC_SA_ESTABLISHED(st->st_state) &&
			    p1st->st_connection->host_pair ==
			      st->st_connection->host_pair &&
			    same_peer_ids(p1st->st_connection,
					  st->st_connection, NULL)) {
				struct ipsec_proto_info *pr =
					protoid == PROTO_IPSEC_AH ?
						&st->st_ah : &st->st_esp;

				if (pr->present) {
					if (pr->attrs.spi == spi)
						return st;

					if (pr->our_spi == spi)
						*bogus = TRUE;
				}
			}
		}
	}
	return NULL;
}

/* Find newest Phase 1 negotiation state object for suitable for connection c
 */
struct state *find_phase1_state(const struct connection *c, lset_t ok_states)
{
	struct state
		*st,
		*best = NULL;
	int i;

	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next) {
			if (LHAS(ok_states, st->st_state) &&
			    c->host_pair == st->st_connection->host_pair &&
			    same_peer_ids(c, st->st_connection, NULL) &&
			    (best == NULL ||
			     best->st_serialno < st->st_serialno))
				best = st;
		}
	}

	return best;
}

void state_eroute_usage(const ip_subnet *ours, const ip_subnet *his,
			unsigned long count, monotime_t nw)
{
	struct state *st;
	int i;

	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next) {
			struct connection *c = st->st_connection;

			/* XXX spd-enum */
			if (IS_IPSEC_SA_ESTABLISHED(st->st_state) &&
			    c->spd.eroute_owner == st->st_serialno &&
			    c->spd.routing == RT_ROUTED_TUNNEL &&
			    samesubnet(&c->spd.this.client, ours) &&
			    samesubnet(&c->spd.that.client, his)) {
				if (st->st_outbound_count != count) {
					st->st_outbound_count = count;
					st->st_outbound_time = nw;
				}
				return;
			}
		}
	}
	DBG(DBG_CONTROL,
	    {
		    char ourst[SUBNETTOT_BUF];
		    char hist[SUBNETTOT_BUF];

		    subnettot(ours, 0, ourst, sizeof(ourst));
		    subnettot(his, 0, hist, sizeof(hist));
		    DBG_log("unknown tunnel eroute %s -> %s found in scan",
			    ourst, hist);
	    });
}

void fmt_list_traffic(struct state *st, char *state_buf,
		      const size_t state_buf_len)
{
	const struct connection *c = st->st_connection;
	char inst[CONN_INST_BUF];
	char traffic_buf[512];

	state_buf[0] = '\0';   /* default to empty */
	traffic_buf[0] = '\0';

	if (IS_IKE_SA(st))
		return; /* ignore non-IPsec states */

	if (!IS_IPSEC_SA_ESTABLISHED(st->st_state))
		return; /* ignore non established states */

	fmt_conn_instance(c, inst);

	{
		char *mode = st->st_esp.present ? "ESP" : st->st_ah.present ? "AH" : st->st_ipcomp.present ? "IPCOMP" : "UNKNOWN";
		char *mbcp = traffic_buf + snprintf(traffic_buf,
				sizeof(traffic_buf) - 1, ", type=%s,  add_time=%lu", mode,  st->st_esp.add_time);

		if (get_sa_info(st, FALSE, NULL)) {
			u_int inb = st->st_esp.present ? st->st_esp.peer_bytes :
				st->st_ah.present ? st->st_ah.peer_bytes :
				st->st_ipcomp.present ? st->st_ipcomp.peer_bytes : 0;
			size_t buf_len =  traffic_buf + sizeof(traffic_buf) - mbcp;

			mbcp += snprintf(mbcp, buf_len - 1, ", inBytes=%u", inb);
		}
		if (get_sa_info(st, TRUE, NULL)) {
			size_t buf_len =  traffic_buf + sizeof(traffic_buf) - mbcp;
			u_int outb = st->st_esp.present ? st->st_esp.our_bytes :
				st->st_ah.present ? st->st_ah.our_bytes :
				st->st_ipcomp.present ? st->st_ipcomp.our_bytes : 0;

			snprintf(mbcp, buf_len - 1, ", outBytes=%u", outb);
		}
	}

	snprintf(state_buf, state_buf_len,
		"#%lu: \"%s\"%s%s%s%s",
		st->st_serialno,
		c->name, inst,
		(st->st_xauth_username[0] != '\0') ? ", XAUTHuser=" : "",
		(st->st_xauth_username[0] != '\0') ? st->st_xauth_username : "",
		(traffic_buf[0] != '\0') ? traffic_buf : ""
		);
}

void fmt_state(struct state *st, const monotime_t n,
	       char *state_buf, const size_t state_buf_len,
	       char *state_buf2, const size_t state_buf2_len)
{
	/* what the heck is interesting about a state? */
	const struct connection *c = st->st_connection;
	long delta;
	char inst[CONN_INST_BUF];
	char dpdbuf[128];
	char traffic_buf[512], *mbcp;
	const char *np1 = c->newest_isakmp_sa == st->st_serialno ?
			  "; newest ISAKMP" : "";
	const char *np2 = c->newest_ipsec_sa == st->st_serialno ?
			  "; newest IPSEC" : "";
	/* XXX spd-enum */
	const char *eo = c->spd.eroute_owner == st->st_serialno ?
			 "; eroute owner" : "";
	const char *idlestr;

	fmt_conn_instance(c, inst);

	if (st->st_event != NULL) {
		/* tricky: in case time_t/monotime_t is an unsigned type */
		delta = monobefore(n, st->st_event->ev_time) ?
			(long)(st->st_event->ev_time.mono_secs - n.mono_secs) :
			-(long)(n.mono_secs - st->st_event->ev_time.mono_secs);
	} else {
		delta = -1;	/* ??? sort of odd signifier */
	}

	dpdbuf[0] = '\0';	/* default to empty string */
	if (IS_IPSEC_SA_ESTABLISHED(st->st_state)) {
		snprintf(dpdbuf, sizeof(dpdbuf), "; isakmp#%lu",
			 (unsigned long)st->st_clonedfrom);
	} else {
		if (st->hidden_variables.st_peer_supports_dpd) {

			/* ??? why is printing -1 better than 0? */
			snprintf(dpdbuf, sizeof(dpdbuf),
				 "; lastdpd=%lds(seq in:%u out:%u)",
				 st->st_last_dpd.mono_secs != UNDEFINED_TIME ?
					(long)deltasecs(monotimediff(mononow(), st->st_last_dpd)) : (long)-1,
				 st->st_dpd_seqno,
				 st->st_dpd_expectseqno);
		} else if (dpd_active_locally(st) && st->st_ikev2) {
			/* stats are on parent sa */
			if (IS_CHILD_SA(st)) {
				struct state *pst = state_with_serialno(st->st_clonedfrom);

				if (pst != NULL) {
					snprintf(dpdbuf, sizeof(dpdbuf),
						"; lastlive=%lds",
						pst->st_last_liveness.mono_secs != UNDEFINED_TIME ?
						deltasecs(monotimediff(mononow(), pst->st_last_liveness)) :
						0);
				}
			}
		} else {
			if (!st->st_ikev2)
				snprintf(dpdbuf, sizeof(dpdbuf), "; nodpd");
		}
	}

	DBG(DBG_CONTROLMORE, DBG_log("#%lu %s:%u st->st_calculating == %s;", st->st_serialno, __FUNCTION__, __LINE__, st->st_calculating ? "TRUE" : "FALSE"));
	if (st->st_calculating)
		idlestr = "crypto_calculating";
	else if (st->st_suspended_md)
		idlestr = "crypto/dns-lookup";
	else
		idlestr = "idle";

	snprintf(state_buf, state_buf_len,
		 "#%lu: \"%s\"%s:%u %s (%s); %s in %lds%s%s%s%s; %s; %s",
		 st->st_serialno,
		 c->name, inst,
		 st->st_remoteport,
		 enum_name(&state_names, st->st_state),
		 state_story[st->st_state - STATE_MAIN_R0],
		 st->st_event ? enum_name(&timer_event_names,
					  st->st_event->ev_type) : "none",
		 delta,
		 np1, np2, eo, dpdbuf,
		 idlestr,
		 enum_name(&pluto_cryptoimportance_names, st->st_import));

	/* print out SPIs if SAs are established */
	if (state_buf2_len != 0)
		state_buf2[0] = '\0';   /* default to empty */
	if (IS_IPSEC_SA_ESTABLISHED(st->st_state)) {
		char lastused[40];      /* should be plenty long enough */
		char buf[SATOT_BUF * 6 + 1];
		char *p = buf;

#	define add_said(adst, aspi, aproto) { \
		ip_said s; \
		\
		initsaid(adst, aspi, aproto, &s); \
		if (p < &buf[sizeof(buf) - 1]) \
		{ \
			*p++ = ' '; \
			p += satot(&s, 0, p, &buf[sizeof(buf)] - p) - 1; \
		} \
}

		/* XXX - mcr last used is really an attribute of the connection */
		lastused[0] = '\0';
		if (c->spd.eroute_owner == st->st_serialno &&
		    st->st_outbound_count != 0) {
			snprintf(lastused, sizeof(lastused),
				 " used %lds ago;",
				 (long) deltasecs(monotimediff(mononow(),
						  st->st_outbound_time)));
		}

		mbcp = traffic_buf +
		       snprintf(traffic_buf, sizeof(traffic_buf) - 1,
				"Traffic:");

		*p = '\0';
		if (st->st_ah.present) {
			add_said(&c->spd.that.host_addr, st->st_ah.attrs.spi,
				 SA_AH);
/* needs proper fix, via kernel_ops? */
#if defined(linux) && defined(NETKEY_SUPPORT)
			if (get_sa_info(st, FALSE, NULL)) {
				mbcp = humanize_number(st->st_ah.peer_bytes,
						       mbcp,
						       traffic_buf +
							  sizeof(traffic_buf),
						       " AHin=");
			}
#endif
			add_said(&c->spd.this.host_addr, st->st_ah.our_spi,
				 SA_AH);
#if defined(linux) && defined(NETKEY_SUPPORT)
			if (get_sa_info(st, TRUE, NULL)) {
				mbcp = humanize_number(st->st_ah.our_bytes,
						       mbcp,
						       traffic_buf +
						         sizeof(traffic_buf),
						       " AHout=");
			}
#endif
			mbcp = humanize_number(
					(u_long)st->st_ah.attrs.life_kilobytes,
					mbcp,
					traffic_buf +
					  sizeof(traffic_buf),
					"! AHmax=");
/* ??? needs proper fix, via kernel_ops? */
		}
		if (st->st_esp.present) {
			add_said(&c->spd.that.host_addr, st->st_esp.attrs.spi,
				 SA_ESP);
/* ??? needs proper fix, via kernel_ops? */
#if defined(linux) && defined(NETKEY_SUPPORT)
			if (get_sa_info(st, FALSE, NULL)) {
				mbcp = humanize_number(st->st_esp.peer_bytes,
						       mbcp,
						       traffic_buf +
							 sizeof(traffic_buf),
						       " ESPin=");
			}
#endif
			add_said(&c->spd.this.host_addr, st->st_esp.our_spi,
				 SA_ESP);
#if defined(linux) && defined(NETKEY_SUPPORT)
			if (get_sa_info(st, TRUE, NULL)) {
				mbcp = humanize_number(st->st_esp.our_bytes,
						       mbcp,
						       traffic_buf +
							 sizeof(traffic_buf),
						       " ESPout=");
			}
#endif

			mbcp = humanize_number(
					(u_long)st->st_esp.attrs.life_kilobytes,
					mbcp,
					traffic_buf +
					  sizeof(traffic_buf),
					"! ESPmax=");
		}
		if (st->st_ipcomp.present) {
			add_said(&c->spd.that.host_addr,
				 st->st_ipcomp.attrs.spi, SA_COMP);
#if defined(linux) && defined(NETKEY_SUPPORT)
			if (get_sa_info(st, FALSE, NULL)) {
				mbcp = humanize_number(
						st->st_ipcomp.peer_bytes,
						mbcp,
						traffic_buf +
						  sizeof(traffic_buf),
						" IPCOMPin=");
			}
#endif
			add_said(&c->spd.this.host_addr, st->st_ipcomp.our_spi,
				 SA_COMP);
#if defined(linux) && defined(NETKEY_SUPPORT)
			if (get_sa_info(st, TRUE, NULL)) {
				mbcp = humanize_number(
						st->st_ipcomp.our_bytes,
						mbcp,
						traffic_buf +
						  sizeof(traffic_buf),
						" IPCOMPout=");
			}
#endif

			/* mbcp not subsequently used */
			mbcp = humanize_number(
					(u_long)st->st_ipcomp.attrs.life_kilobytes,
					mbcp,
					traffic_buf + sizeof(traffic_buf),
					"! IPCOMPmax=");
		}
#ifdef KLIPS
		if (st->st_ah.attrs.encapsulation ==
		      ENCAPSULATION_MODE_TUNNEL ||
		    st->st_esp.attrs.encapsulation ==
		      ENCAPSULATION_MODE_TUNNEL ||
		    st->st_ipcomp.attrs.encapsulation ==
		      ENCAPSULATION_MODE_TUNNEL) {
			add_said(&c->spd.that.host_addr, st->st_tunnel_out_spi,
				 SA_IPIP);
			add_said(&c->spd.this.host_addr, st->st_tunnel_in_spi,
				 SA_IPIP);
		}
#endif

		snprintf(state_buf2, state_buf2_len,
			 "#%lu: \"%s\"%s%s%s ref=%lu refhim=%lu %s %s%s",
			 st->st_serialno,
			 c->name, inst,
			 lastused,
			 buf,
			 (unsigned long)st->st_ref,
			 (unsigned long)st->st_refhim,
			 traffic_buf,
			 (st->st_xauth_username[0] != '\0') ? "XAUTHuser=" : "",
			 (st->st_xauth_username[0] != '\0') ? st->st_xauth_username : "");

#       undef add_said
	}
}

/*
 * sorting logic is:
 *
 *  name
 *  type
 *  instance#
 *  isakmp_sa (XXX probably wrong)
 *
 */
static int state_compare(const void *a, const void *b)
{
	const struct state *sap = *(const struct state *const *)a;
	struct connection *ca = sap->st_connection;
	const struct state *sbp = *(const struct state *const *)b;
	struct connection *cb = sbp->st_connection;

	/* DBG_log("comparing %s to %s", ca->name, cb->name); */

	return connection_compare(ca, cb);
}

void show_states_status(bool list_traffic)
{
	int i;
	int count;

	if (list_traffic) {
		whack_log(RC_COMMENT, " ");             /* spacer */
	}	else {
		whack_log(RC_COMMENT, " ");             /* spacer */
		whack_log(RC_COMMENT, "State list:");   /* spacer */
		whack_log(RC_COMMENT, " ");             /* spacer */
	}

	/* make count of states */
	count = 0;
	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		struct state *st;

		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next)
			count++;
	}

	if (count != 0) {
		monotime_t n = mononow();
#if 1
		/* C99's VLA feature is just what we need */
		struct state *array[count];
#else
		/* no VLA: use alloca (ouch!) */
		struct state **array = alloca(sizeof(struct state *) * count);
#endif

		/* build the array */
		count = 0;
		for (i = 0; i < STATE_TABLE_SIZE; i++) {
			struct state *st;

			for (st = statetable[i]; st != NULL;
			     st = st->st_hashchain_next)
				array[count++] = st;
		}

		/* sort it! */
		qsort(array, count, sizeof(struct state *), state_compare);

		/* now print sorted results */
		for (i = 0; i < count; i++) {
			char state_buf[LOG_WIDTH];
			char state_buf2[LOG_WIDTH];
			struct state *st = array[i];

			if (list_traffic) {
				fmt_list_traffic(st, state_buf,
						sizeof(state_buf));
				if (state_buf[0] != '\0')
					whack_log(RC_INFORMATIONAL_TRAFFIC, "%s", state_buf);
			} else {

				fmt_state(st, n, state_buf, sizeof(state_buf),
						state_buf2, sizeof(state_buf2));
				whack_log(RC_COMMENT, "%s", state_buf);
				if (state_buf2[0] != '\0')
					whack_log(RC_COMMENT, "%s", state_buf2);

				/* show any associated pending Phase 2s */
				if (IS_IKE_SA(st))
					show_pending_phase2(st->st_connection, st);
			}
		}

		whack_log(RC_COMMENT, " "); /* spacer */
	}
}

/* Given that we've used up a range of unused CPI's,
 * search for a new range of currently unused ones.
 * Note: this is very expensive when not trivial!
 * If we can't find one easily, choose 0 (a bad SPI,
 * no matter what order) indicating failure.
 */
void find_my_cpi_gap(cpi_t *latest_cpi, cpi_t *first_busy_cpi)
{
	int tries = 0;
	cpi_t base = *latest_cpi;
	cpi_t closest;
	int i;

startover:
	closest = ~0;   /* not close at all */
	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		struct state *st;

		for (st = statetable[i]; st != NULL;
		     st = st->st_hashchain_next) {
			if (st->st_ipcomp.present) {
				cpi_t c = ntohl(st->st_ipcomp.our_spi) - base;

				if (c < closest) {
					if (c == 0) {
						/* oops: next spot is occupied; start over */
						if (++tries == 20) {
							/* FAILURE */
							*latest_cpi =
								*first_busy_cpi
									= 0;
							return;
						}
						base++;
						if (base >
						    IPCOMP_LAST_NEGOTIATED)
							base = IPCOMP_FIRST_NEGOTIATED;

						goto startover; /* really a tail call */
					}
					closest = c;
				}
			}
		}
	}
	*latest_cpi = base;                     /* base is first in next free range */
	*first_busy_cpi = closest + base;       /* and this is the roof */
}

/* Muck with high-order 16 bits of this SPI in order to make
 * the corresponding SAID unique.
 * Its low-order 16 bits hold a well-known IPCOMP CPI.
 * Oh, and remember that SPIs are stored in network order.
 * Kludge!!!  So I name it with the non-English word "uniquify".
 * If we can't find one easily, return 0 (a bad SPI,
 * no matter what order) indicating failure.
 */
ipsec_spi_t uniquify_his_cpi(ipsec_spi_t cpi, const struct state *st)
{
	int tries = 0;
	int i;

startover:

	/* network order makes first two bytes our target */
	get_rnd_bytes((u_char *)&cpi, 2);

	/* Make sure that the result is unique.
	 * Hard work.  If there is no unique value, we'll loop forever!
	 */
	for (i = 0; i < STATE_TABLE_SIZE; i++) {
		const struct state *s;

		for (s = statetable[i]; s != NULL; s = s->st_hashchain_next) {
			if (s->st_ipcomp.present &&
			    sameaddr(&s->st_connection->spd.that.host_addr,
				     &st->st_connection->spd.that.host_addr) &&
			    cpi == s->st_ipcomp.attrs.spi) {
				if (++tries == 20)
					return 0; /* FAILURE */

				goto startover;
			}
		}
	}
	return cpi;
}

void merge_quirks(struct state *st, const struct msg_digest *md)
{
	struct isakmp_quirks *dq = &st->quirks;
	const struct isakmp_quirks *sq = &md->quirks;

	dq->xauth_ack_msgid   |= sq->xauth_ack_msgid;
	dq->modecfg_pull_mode |= sq->modecfg_pull_mode;
	/* ??? st->quirks.qnat_traversal is never used */
	if (dq->qnat_traversal_vid < sq->qnat_traversal_vid)
		dq->qnat_traversal_vid = sq->qnat_traversal_vid;
	dq->xauth_vid |= sq->xauth_vid;
}

void set_state_ike_endpoints(struct state *st,
			     struct connection *c)
{
	/* reset our choice of interface */
	c->interface = NULL;
	orient(c);

	st->st_localaddr  = c->spd.this.host_addr;
	st->st_localport  = c->spd.this.host_port;
	st->st_remoteaddr = c->spd.that.host_addr;
	st->st_remoteport = c->spd.that.host_port;

	st->st_interface = c->interface;
}

/* seems to be a good spot for now */
bool dpd_active_locally(const struct state *st)
{
	return deltasecs(st->st_connection->dpd_delay) != 0 &&
		deltasecs(st->st_connection->dpd_timeout) != 0;
}

void delete_my_family(struct state *pst, bool v2_responder_state)
{
	/* We are a parent: delete our children and
	 * then prepare to delete ourself.
	 * Our children will be on the same hash chain
	 * because we share IKE SPIs.
	 */
	struct state *st;

	passert(!IS_CHILD_SA(pst));	/* we had better be a parent */

	/* find first in chain */
	for (st = pst; st->st_hashchain_prev != NULL; )
		st = st->st_hashchain_prev;

	/* delete each of our children */
	while (st != NULL) {
		/* since we might be deleting st, we need to
		 * grab onto its successor first
		 */
		struct state *next_st = st->st_hashchain_next;

		if (st->st_clonedfrom == pst->st_serialno) {
			if (v2_responder_state)
				change_state(st, STATE_CHILDSA_DEL);
			delete_state(st);
		}
		st = next_st;
		/* note: no md->st to clear */
	}

	/* delete self */
	if (v2_responder_state)
		change_state(pst, STATE_IKESA_DEL);
	delete_state(pst);
	/* note: no md->st to clear */
}

/* if the state is too busy to process a packet, say so */
bool state_busy(const struct state *st) {
	if (st != NULL) {
		/* Ignore a packet if the state has a suspended state transition
		 * Probably a duplicated packet but the original packet is not yet
		 * recorded in st->st_rpacket, so duplicate checking won't catch.
		 * ??? Should the packet be recorded earlier to improve diagnosis?
		 */
		if (st->st_suspended_md != NULL) {
			loglog(RC_LOG,
			       "discarding packet received during asynchronous work (DNS or crypto) in %s",
			       enum_name(&state_names, st->st_state));
			return TRUE;
		}

		/*
		 * if this state is busy calculating in between state transitions,
		 * (there will be no suspended state), then we silently ignore the
		 * packet, as there is nothing we can do right now.
		 */
		DBG(DBG_CONTROLMORE, DBG_log("#%lu %s:%u st != NULL && st->st_calculating == %s;", st->st_serialno, __FUNCTION__, __LINE__, st != NULL && st->st_calculating ? "TRUE" : "FALSE"));
		if (st->st_calculating) {
			libreswan_log("message received while calculating. Ignored.");
			return TRUE;
		}
	}
	return FALSE;
}

void clear_dh_from_state(struct state *st)
{
	/* when responding with INVALID_DH, we didn't do the work yet */
	if (st->st_sec_in_use) {
		SECKEY_DestroyPublicKey(st->st_pubk_nss);
		SECKEY_DestroyPrivateKey(st->st_sec_nss);
		st->st_sec_in_use = FALSE;
	}
}
