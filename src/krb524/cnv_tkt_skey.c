/*
 * Copyright 1994 by OpenVision Technologies, Inc.
 * 
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appears in all copies and
 * that both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of OpenVision not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission. OpenVision makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 * 
 * OPENVISION DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL OPENVISION BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "k5-int.h"		/* we need krb5_context::clockskew */
#include <stdio.h>
#include <sys/types.h>

#ifndef _WIN32
#include <sys/time.h>
#include <netinet/in.h>
#endif

#include <krb.h>
#include "krb524.h"

static
int
krb524int_krb_create_ticket(tkt, flags, pname, pinstance, prealm, paddress,
			    session, life, time_sec, sname, sinstance, key, k5key);

static
int
krb524int_krb_cr_tkt_krb5(tkt, flags, pname, pinstance, prealm, paddress,
			  session, life, time_sec, sname, sinstance, k5key);

static int
krb524int_krb_cr_tkt_int(tkt, flags, pname, pinstance, prealm, paddress,
			 session, life, time_sec, sname, sinstance, key, k5key);

/* rather than copying the cmu code, these values are derived from
   a calculation based on the table and comments found there.
   the expression (in elisp) is:
   (defun cmu-to-secs2 (j)
      (if (< j 128) (* j 5 60)
         (round (* 38400 (expt 1.06914489 (- j 128))))))
   and is low by one for 16 values but is exact for the others.
 */

static long cmu_seconds[] = 
{
  38400,  41055,  43894,  46929,  50174,  53643,  57352,  61318,
  65558,  70091,  74937,  80119,  85658,  91581,  97914,  104684,
  111922,  119661,  127935,  136781,  146239,  156350,  167161,  178720,
  191077,  204289,  218415,  233517,  249663,  266926,  285383,  305116,
  326213,  348769,  372885,  398668,  426233,  455705,  487215,  520903,
  556921,  595430,  636600,  680618,  727679,  777995,  831789,  889303,
  950794,  1016536,  1086825,  1161973,  1242317,  1328217,  1420057,  1518246,
  1623225,  1735463,  1855462,  1983757,  2120924,  2267575,  2424366, 2591999,
  0
};

/*
 * Convert a v5 ticket for server to a v4 ticket, using service key
 * skey for both.
 */
int krb524_convert_tkt_skey(context, v5tkt, v4tkt, v5_skey, v4_skey,
			    saddr)
     krb5_context context;
     krb5_ticket *v5tkt;
     KTEXT_ST *v4tkt;
     krb5_keyblock *v5_skey, *v4_skey;
     struct sockaddr_in *saddr;
{
     char pname[ANAME_SZ], pinst[INST_SZ], prealm[REALM_SZ];
     char sname[ANAME_SZ], sinst[INST_SZ];
     krb5_enc_tkt_part *v5etkt;
     int ret, lifetime, deltatime;
     krb5_timestamp server_time;
     struct sockaddr_in *sinp = (struct sockaddr_in *)saddr;
     krb5_address kaddr;

     v5tkt->enc_part2 = NULL;
     if ((ret = krb5_decrypt_tkt_part(context, v5_skey, v5tkt))) {
	  krb5_free_ticket(context, v5tkt);
	  return ret;
     }
     v5etkt = v5tkt->enc_part2;

     if ((ret = krb524_convert_princs(context, v5etkt->client, v5tkt->server,
				     pname, pinst, prealm, sname,
				     sinst))) {
	  krb5_free_enc_tkt_part(context, v5etkt);
	  v5tkt->enc_part2 = NULL;
	  return ret;
     }
     
     if (v5etkt->session->enctype != ENCTYPE_DES_CBC_CRC ||
	 v5etkt->session->length != sizeof(C_Block)) {
	  if (krb524_debug)
	       fprintf(stderr, "v5 session keyblock type %d length %d != C_Block size %d\n",
		       v5etkt->session->enctype,
		       v5etkt->session->length,
		       sizeof(C_Block));
	  krb5_free_enc_tkt_part(context, v5etkt);
	  v5tkt->enc_part2 = NULL;
	  return KRB524_BADKEY;
     }
     
     /* V4 has no concept of authtime or renew_till, so ignore them */
     /* V4 lifetime is 1 byte, in 5 minute increments */
     if (v5etkt->times.starttime == 0)
	  v5etkt->times.starttime = v5etkt->times.authtime;
     /* rather than apply fit an extended v5 lifetime into a v4 range,
	give out a v4 ticket with as much of the v5 lifetime is available
	"now" instead. */
     if ((ret = krb5_timeofday(context, &server_time))) {
         if (krb524_debug)
	      fprintf(stderr, "krb5_timeofday failed!\n");
	 krb5_free_enc_tkt_part(context, v5etkt);
	 v5tkt->enc_part2 = NULL;
	 return ret;       
     }
     if (   (server_time+context->clockskew >= v5etkt->times.starttime)
	 && (server_time-context->clockskew <= v5etkt->times.endtime)) {
          deltatime = v5etkt->times.endtime - (server_time-context->clockskew);
	  lifetime = deltatime / 300;
	  /* if (lifetime > 255) lifetime = 255; */
	  if (lifetime > 127) {
	      /* use the CMU algorithm instead: */
	      long *clist = cmu_seconds;
	      while(*clist && *clist < deltatime) clist++;
	      lifetime = 128 + (clist - cmu_seconds);
	  }
     } else {
          if (krb524_debug)
	       fprintf(stderr, "v5 ticket time out of bounds\n");
	  krb5_free_enc_tkt_part(context, v5etkt);
	  v5tkt->enc_part2 = NULL;
	  if (server_time+context->clockskew < v5etkt->times.starttime)
	       return KRB5KRB_AP_ERR_TKT_NYV;
	  else if (server_time-context->clockskew > v5etkt->times.endtime)
	       return KRB5KRB_AP_ERR_TKT_EXPIRED;
	  else /* shouldn't happen, but just in case... */
	    return KRB5KRB_AP_ERR_TKT_NYV;
     }

     kaddr.addrtype = ADDRTYPE_INET;
     kaddr.length = sizeof(sinp->sin_addr);
     kaddr.contents = (krb5_octet *)&sinp->sin_addr;

     if (!krb5_address_search(context, &kaddr, v5etkt->caddrs)) {
	 if (krb524_debug)
	     fprintf(stderr, "Invalid v5creds address information.\n");
	 krb5_free_enc_tkt_part(context, v5etkt);
	 v5tkt->enc_part2 = NULL;
	 return KRB524_BADADDR;
     }

     if (krb524_debug)
	printf("startime = %ld, authtime = %ld, lifetime = %ld\n",
	       (long) v5etkt->times.starttime,
	       (long) v5etkt->times.authtime,
	       (long) lifetime);

     /* XXX are there V5 flags we should map to V4 equivalents? */
     if (v4_skey->enctype == ENCTYPE_DES_CBC_CRC) {
	 ret = krb524int_krb_create_ticket(v4tkt,
				 0, /* flags */			     
				 pname,
				 pinst,
				 prealm,
				 *((unsigned long *)kaddr.contents),
				 (char *) v5etkt->session->contents,
				 lifetime,
				 /* issue_data */
				 server_time,
				 sname,
				 sinst,
				 v4_skey->contents);
     } else {
	 /* Force enctype to be raw if using DES3. */
	 if (v4_skey->enctype == ENCTYPE_DES3_CBC_SHA1 ||
	     v4_skey->enctype == ENCTYPE_LOCAL_DES3_HMAC_SHA1)
	     v4_skey->enctype = ENCTYPE_DES3_CBC_RAW;
	 ret = krb524int_krb_cr_tkt_krb5(v4tkt,
			       0, /* flags */			     
			       pname,
			       pinst,
			       prealm,
			       *((unsigned long *)kaddr.contents),
			       (char *) v5etkt->session->contents,
			       lifetime,
			       /* issue_data */
			       server_time,
			       sname,
			       sinst,
			       v4_skey);
     }

     krb5_free_enc_tkt_part(context, v5etkt);
     v5tkt->enc_part2 = NULL;
     if (ret == KSUCCESS)
	  return 0;
     else
	  return KRB524_V4ERR;
}

/*****************************************************************************
 * Copied from krb4's cr_tkt.
 * Modified functions below to be static.
 *****************************************************************************/

#define          HOST_BYTE_ORDER (* (char *) &temp_ONE)
static const int temp_ONE = 1;

/*
 * Create ticket takes as arguments information that should be in a
 * ticket, and the KTEXT object in which the ticket should be
 * constructed.  It then constructs a ticket and returns, leaving the
 * newly created ticket in tkt.
#ifndef NOENCRYPTION
 * The data in tkt->dat is encrypted in the server's key.
#endif
 * The length of the ticket is a multiple of
 * eight bytes and is in tkt->length.
 *
 * If the ticket is too long, the ticket will contain nulls.
 * The return value of the routine is undefined.
 *
 * The corresponding routine to extract information from a ticket it
 * decomp_ticket.  When changes are made to this routine, the
 * corresponding changes should also be made to that file.
 *
 * The packet is built in the following format:
 * 
 * 			variable
 * type			or constant	   data
 * ----			-----------	   ----
 *
 * tkt->length		length of ticket (multiple of 8 bytes)
 * 
#ifdef NOENCRYPTION
 * tkt->dat:
#else
 * tkt->dat:		(encrypted in server's key)
#endif
 * 
 * unsigned char	flags		   namely, HOST_BYTE_ORDER
 * 
 * string		pname		   client's name
 * 
 * string		pinstance	   client's instance
 * 
 * string		prealm		   client's realm
 * 
 * 4 bytes		paddress	   client's address
 * 
 * 8 bytes		session		   session key
 * 
 * 1 byte		life		   ticket lifetime
 * 
 * 4 bytes		time_sec	   KDC timestamp
 * 
 * string		sname		   service's name
 * 
 * string		sinstance	   service's instance
 * 
 * <=7 bytes		null		   null pad to 8 byte multiple
 *
 */
static
int
krb524int_krb_create_ticket(tkt, flags, pname, pinstance, prealm, paddress,
		  session, life, time_sec, sname, sinstance, key, k5key)
    KTEXT   tkt;                /* Gets filled in by the ticket */
    unsigned char flags;        /* Various Kerberos flags */
    char    *pname;             /* Principal's name */
    char    *pinstance;         /* Principal's instance */
    char    *prealm;            /* Principal's authentication domain */
    long    paddress;           /* Net address of requesting entity */
    char    *session;           /* Session key inserted in ticket */
    short   life;               /* Lifetime of the ticket */
    long    time_sec;           /* Issue time and date */
    char    *sname;             /* Service Name */
    char    *sinstance;         /* Instance Name */
    C_Block key;                /* Service's secret key */
{
    return krb524int_krb_cr_tkt_int(tkt, flags, pname, pinstance, prealm,
				    paddress, session, life, time_sec, sname,
				    sinstance, key, NULL);
}

static
int
krb524int_krb_cr_tkt_krb5(tkt, flags, pname, pinstance, prealm, paddress,
		  session, life, time_sec, sname, sinstance, k5key)
    KTEXT   tkt;                /* Gets filled in by the ticket */
    unsigned char flags;        /* Various Kerberos flags */
    char    *pname;             /* Principal's name */
    char    *pinstance;         /* Principal's instance */
    char    *prealm;            /* Principal's authentication domain */
    long    paddress;           /* Net address of requesting entity */
    char    *session;           /* Session key inserted in ticket */
    short   life;               /* Lifetime of the ticket */
    long    time_sec;           /* Issue time and date */
    char    *sname;             /* Service Name */
    char    *sinstance;         /* Instance Name */
    krb5_keyblock *k5key;	/* NULL if not present */
{
    C_Block key;

    return krb524int_krb_cr_tkt_int(tkt, flags, pname, pinstance, prealm,
				    paddress, session, life, time_sec, sname,
				    sinstance, key, k5key);
}

static int
krb524int_krb_cr_tkt_int(tkt, flags, pname, pinstance, prealm, paddress,
	       session, life, time_sec, sname, sinstance, key, k5key)
    KTEXT   tkt;                /* Gets filled in by the ticket */
    unsigned char flags;        /* Various Kerberos flags */
    char    *pname;             /* Principal's name */
    char    *pinstance;         /* Principal's instance */
    char    *prealm;            /* Principal's authentication domain */
    long    paddress;           /* Net address of requesting entity */
    char    *session;           /* Session key inserted in ticket */
    short   life;               /* Lifetime of the ticket */
    long    time_sec;           /* Issue time and date */
    char    *sname;             /* Service Name */
    char    *sinstance;         /* Instance Name */
    C_Block key;                /* Service's secret key */
    krb5_keyblock *k5key;	/* NULL if not present */
{
    Key_schedule key_s;
    register char *data;        /* running index into ticket */

    tkt->length = 0;            /* Clear previous data  */

    /* Check length of ticket */
    if (sizeof(tkt->dat) < (sizeof(flags) +
                            1 + strlen(pname) +
                            1 + strlen(pinstance) +
                            1 + strlen(prealm) +
                            4 +                         /* address */
			    8 +                         /* session */
			    1 +                         /* life */
			    4 +                         /* issue time */
                            1 + strlen(sname) +
                            1 + strlen(sinstance) +
			    7) / 8) {                   /* roundoff */
        memset(tkt->dat, 0, sizeof(tkt->dat));
        return KFAILURE /* XXX */;
    }

    flags |= HOST_BYTE_ORDER;   /* ticket byte order   */
    memcpy((char *) (tkt->dat), (char *) &flags, sizeof(flags));
    data = ((char *)tkt->dat) + sizeof(flags);
    (void) strcpy(data, pname);
    data += 1 + strlen(pname);
    (void) strcpy(data, pinstance);
    data += 1 + strlen(pinstance);
    (void) strcpy(data, prealm);
    data += 1 + strlen(prealm);
    memcpy(data, (char *) &paddress, 4);
    data += 4;

    memcpy(data, (char *) session, 8);
    data += 8;
    *(data++) = (char) life;
    /* issue time */
    memcpy(data, (char *) &time_sec, 4);
    data += 4;
    (void) strcpy(data, sname);
    data += 1 + strlen(sname);
    (void) strcpy(data, sinstance);
    data += 1 + strlen(sinstance);

    /* guarantee null padded ticket to multiple of 8 bytes */
    memset(data, 0, 7);
    tkt->length = ((data - ((char *)tkt->dat) + 7)/8)*8;

    /* Check length of ticket */
    if (tkt->length > (sizeof(KTEXT_ST) - 7)) {
        memset(tkt->dat, 0, tkt->length);
        tkt->length = 0;
        return KFAILURE /* XXX */;
    }

#ifndef NOENCRYPTION
    /* Encrypt the ticket in the services key */
    if (k5key != NULL) {
	/* block locals */
	krb5_data in;
	krb5_enc_data out;
	krb5_error_code ret;
	size_t enclen;

	in.length = tkt->length;
	in.data = tkt->dat;
	/* XXX assumes context arg is ignored */
	ret = krb5_c_encrypt_length(NULL, k5key->enctype,
				    (size_t)in.length, &enclen);
	if (ret)
	    return KFAILURE;
	out.ciphertext.length = enclen;
	out.ciphertext.data = malloc(enclen);
	if (out.ciphertext.data == NULL)
	    return KFAILURE;	/* XXX maybe ENOMEM? */

	/* XXX assumes context arg is ignored */
	ret = krb5_c_encrypt(NULL, k5key, KRB5_KEYUSAGE_KDC_REP_TICKET,
			     NULL, &in, &out);
	if (ret) {
	    free(out.ciphertext.data);
	    return KFAILURE;
	} else {
	    tkt->length = out.ciphertext.length;
	    memcpy(tkt->dat, out.ciphertext.data, out.ciphertext.length);
	    memset(out.ciphertext.data, 0, out.ciphertext.length);
	    free(out.ciphertext.data);
	}
    } else {
	key_sched(key,key_s);
	pcbc_encrypt((C_Block *)tkt->dat,(C_Block *)tkt->dat,
		     (long) tkt->length,key_s,(C_Block *)key,1);
    }
#endif /* !NOENCRYPTION */
    return 0;
}
