#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <com_err.h>

#define TEST
#include "locate_kdc.c"

enum {
    LOOKUP_CONF = 3,
    LOOKUP_DNS,
    LOOKUP_WHATEVER
} how = LOOKUP_WHATEVER;

const char *prog;

struct addrlist al;

void kfatal (krb5_error_code err)
{
    com_err (prog, err, "- exiting");
    exit (1);
}

const char *stypename (int stype)
{
    static char buf[20];
    switch (stype) {
    case SOCK_STREAM:
	return "stream";
    case SOCK_DGRAM:
	return "dgram";
    case SOCK_RAW:
	return "raw";
    default:
	sprintf(buf, "?%d", stype);
	return buf;
    }
}

void print_addrs ()
{
    int i;

    struct addrinfo **addrs = al.addrs;
    int naddrs = al.naddrs;

    printf ("%d addresses:\n", naddrs);
    for (i = 0; i < naddrs; i++) {
	int err;
	char hostbuf[NI_MAXHOST], srvbuf[NI_MAXSERV];
	err = getnameinfo (addrs[i]->ai_addr, addrs[i]->ai_addrlen,
			   hostbuf, sizeof (hostbuf),
			   srvbuf, sizeof (srvbuf),
			   NI_NUMERICHOST | NI_NUMERICSERV);
	if (err)
	    printf ("%2d: getnameinfo returns error %d=%s\n",
		    i, err, gai_strerror (err));
	else
	    printf ("%2d: address %s\t%s\tport %s\n", i, hostbuf,
		    stypename (addrs[i]->ai_socktype), srvbuf);
    }
}

int main (int argc, char *argv[])
{
    char *p, *realmname;
    krb5_data realm;
    krb5_context ctx;
    krb5_error_code err;

    p = strrchr (argv[0], '/');
    if (p)
	prog = p+1;
    else
	prog = argv[0];

    switch (argc) {
    case 2:
	/* foo $realm */
	realmname = argv[1];
	break;
    case 3:
	if (!strcmp (argv[1], "-c"))
	    how = LOOKUP_CONF;
	else if (!strcmp (argv[1], "-d"))
	    how = LOOKUP_DNS;
	else
	    goto usage;
	realmname = argv[2];
	break;
    default:
    usage:
	fprintf (stderr, "%s: usage: %s [-c | -d] realm\n", prog, prog);
	return 1;
    }

    err = krb5_init_context (&ctx);
    if (err)
	kfatal (err);

    realm.data = realmname;
    realm.length = strlen (realmname);

    switch (how) {
    case LOOKUP_CONF:
	err = krb5_locate_srv_conf (ctx, &realm, "kdc", &al, 0,
				    htons (88), htons (750));
	break;

    case LOOKUP_DNS:
	err = krb5_locate_srv_dns (&realm, "_kerberos", "_udp", &al);
	break;

    case LOOKUP_WHATEVER:
	err = krb5_locate_kdc (ctx, &realm, &al, 0, 0);
	break;
    }
    if (err) kfatal (err);
    print_addrs ();
    return 0;
}
