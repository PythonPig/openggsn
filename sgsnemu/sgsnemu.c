/* 
 *  OpenGGSN - Gateway GPRS Support Node
 *  Copyright (C) 2002, 2003 Mondru AB.
 * 
 *  The contents of this file may be used under the terms of the GNU
 *  General Public License Version 2, provided that the above copyright
 *  notice and this permission notice is included in all copies or
 *  substantial portions of the software.
 * 
 *  The initial developer of the original code is
 *  Jens Jakobsen <jj@openggsn.org>
 * 
 *  Contributor(s):
 * 
 */

/*
 * sgsnemu.c
 *
 */


#ifdef __linux__
#define _GNU_SOURCE 1 /* strdup() prototype, broken arpa/inet.h */
#endif


#include <syslog.h>
#include <ctype.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <features.h>
#include <errno.h>
#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <resolv.h>
#include <time.h>

#include "tun.h"
#include "ippool.h"
#include "syserr.h"
#include "../gtp/pdp.h"
#include "../gtp/gtp.h"
#include "cmdline.h"

#define IPADDRLEN 256      /* Character length of addresses */ 
#define MAXCONTEXTS 16     /* Max number of allowed contexts */ 

/* HASH tables for IP address allocation */
struct iphash_t {
  uint8_t     inuse;    /* 0=free. 1=used by somebody */
  struct iphash_t *ipnext;
  struct pdp_t *pdp;
  struct in_addr addr;
};
struct iphash_t iparr[MAXCONTEXTS];
struct iphash_t *iphash[MAXCONTEXTS];

/* State variable used for ping  */
/* 0: Idle                       */
/* 1: Wait_connect               */
/* 2: Connected                  */
/* 3: Wait_disconnect            */
int state = 0;                  

struct gsn_t *gsn = NULL;       /* GSN instance */
struct tun_t *tun = NULL;       /* TUN instance */
int maxfd = 0;	                /* For select() */

/* Struct with local versions of gengetopt options */
struct {
  int debug;                      /* Print debug messages */
  int createif;                   /* Create local network interface */
  char *ipup, *ipdown;            /* Filename of scripts */
  int defaultroute;               /* Set up default route */
  struct in_addr pinghost;        /* Remote ping host    */
  int pingrate;
  int pingsize;
  int pingcount;
  int pingquiet;
  struct in_addr listen;
  struct in_addr remote;
  struct in_addr dns;
  int contexts;                   /* Number of contexts to create */
  int timelimit;                  /* Number of seconds to be connected */
  char *statedir;
  uint64_t imsi;
  struct ul255_t pco;
  struct ul255_t qos;
  struct ul255_t apn;
  struct ul16_t msisdn;
} options;


/* Definitions to use for PING. Most of the ping code was derived from */
/* the original ping program by Mike Muuss                             */

/* IP header and ICMP echo header */
#define CREATEPING_MAX  2048
#define CREATEPING_IP     20
#define CREATEPING_ICMP    8

struct ip_ping {
  u_int8_t ipver;               /* Type and header length*/
  u_int8_t tos;                 /* Type of Service */
  u_int16_t length;             /* Total length */
  u_int16_t fragid;             /* Identifier */
  u_int16_t offset;             /* Flags and fragment offset */
  u_int8_t ttl;                 /* Time to live */
  u_int8_t protocol;            /* Protocol */
  u_int16_t ipcheck;            /* Header checksum */
  u_int32_t src;                /* Source address */
  u_int32_t dst;                /* Destination */
  u_int8_t type;                /* Type and header length*/
  u_int8_t code;                /* Code */
  u_int16_t checksum;           /* Header checksum */
  u_int16_t ident;              /* Identifier */
  u_int16_t seq;                /* Sequence number */
  u_int8_t data[CREATEPING_MAX]; /* Data */
} __attribute__((packed));

/* Statistical values for ping */
int nreceived = 0;
int ntreceived = 0;
int ntransmitted = 0;
int tmin = 999999999;
int tmax = 0;
int tsum = 0;
int pingseq = 0;              /* Ping sequence counter */
struct timeval firstping;

int ipset(struct iphash_t *ipaddr, struct in_addr *addr) {
  int hash = ippool_hash4(addr) % MAXCONTEXTS;
  struct iphash_t *h;
  struct iphash_t *prev = NULL;
  ipaddr->ipnext = NULL;
  ipaddr->addr.s_addr = addr->s_addr;
  for (h = iphash[hash]; h; h = h->ipnext)
    prev = h;
  if (!prev) 
    iphash[hash] = ipaddr;
  else 
    prev->ipnext = ipaddr;
  return 0;
}

int ipdel(struct iphash_t *ipaddr) {
  int hash = ippool_hash4(&ipaddr->addr) % MAXCONTEXTS;
  struct iphash_t *h;
  struct iphash_t *prev = NULL;
  for (h = iphash[hash]; h; h = h->ipnext) {
    if (h == ipaddr) {
      if (!prev) 
	iphash[hash] = h->ipnext;
      else 
	prev->ipnext = h->ipnext;
      return 0;
    }
    prev = h;
  }
  return EOF; /* End of linked list and not found */
}

int ipget(struct iphash_t **ipaddr, struct in_addr *addr) {
  int hash = ippool_hash4(addr) % MAXCONTEXTS;
  struct iphash_t *h;
  for (h = iphash[hash]; h; h = h->ipnext) {
    if ((h->addr.s_addr == addr->s_addr)) {
      *ipaddr = h;
      return 0;
    }
  }
  return EOF; /* End of linked list and not found */
}


/* Used to write process ID to file. Assume someone else will delete */
void log_pid(char *pidfile) {
  FILE *file;
  mode_t oldmask;
  
  oldmask = umask(022);
  file = fopen(pidfile, "w");
  umask(oldmask);
  if(!file)
    return;
  fprintf(file, "%d\n", getpid());
  fclose(file);
}


int process_options(int argc, char **argv) {
  /* gengeopt declarations */
  struct gengetopt_args_info args_info;

  struct hostent *host;
  int n;

  if (cmdline_parser (argc, argv, &args_info) != 0)
    return -1;
  if (args_info.debug_flag) {
    printf("remote: %s\n", args_info.remote_arg);
    printf("listen: %s\n", args_info.listen_arg);
    printf("conf: %s\n", args_info.conf_arg);
    printf("fg: %d\n", args_info.fg_flag);
    printf("debug: %d\n", args_info.debug_flag);
    printf("imsi: %s\n", args_info.imsi_arg);
    printf("qos: %#08x\n", args_info.qos_arg);
    printf("apn: %s\n", args_info.apn_arg);
    printf("msisdn: %s\n", args_info.msisdn_arg);
    printf("uid: %s\n", args_info.uid_arg);
    printf("pwd: %s\n", args_info.pwd_arg);
    printf("pidfile: %s\n", args_info.pidfile_arg);
    printf("statedir: %s\n", args_info.statedir_arg);
    printf("dns: %s\n", args_info.dns_arg);
    printf("contexts: %d\n", args_info.contexts_arg);
    printf("timelimit: %d\n", args_info.timelimit_arg);
    printf("createif: %d\n", args_info.createif_flag);
    printf("ipup: %s\n", args_info.ipup_arg);
    printf("ipdown: %s\n", args_info.ipdown_arg);
    printf("defaultroute: %d\n", args_info.defaultroute_flag);
    printf("pinghost: %s\n", args_info.pinghost_arg);
    printf("pingrate: %d\n", args_info.pingrate_arg);
    printf("pingsize: %d\n", args_info.pingsize_arg);
    printf("pingcount: %d\n", args_info.pingcount_arg);
    printf("pingquiet: %d\n", args_info.pingquiet_flag);
  }

  /* Try out our new parser */
  
  if (args_info.conf_arg) {
    if (cmdline_parser_configfile (args_info.conf_arg, &args_info, 0) != 0)
      return -1;
    if (args_info.debug_flag) {
      printf("cmdline_parser_configfile\n");
      printf("remote: %s\n", args_info.remote_arg);
      printf("listen: %s\n", args_info.listen_arg);
      printf("conf: %s\n", args_info.conf_arg);
      printf("fg: %d\n", args_info.fg_flag);
      printf("debug: %d\n", args_info.debug_flag);
      printf("imsi: %s\n", args_info.imsi_arg);
      printf("qos: %#08x\n", args_info.qos_arg);
      printf("apn: %s\n", args_info.apn_arg);
      printf("msisdn: %s\n", args_info.msisdn_arg);
      printf("uid: %s\n", args_info.uid_arg);
      printf("pwd: %s\n", args_info.pwd_arg);
      printf("pidfile: %s\n", args_info.pidfile_arg);
      printf("statedir: %s\n", args_info.statedir_arg);
      printf("dns: %s\n", args_info.dns_arg);
      printf("contexts: %d\n", args_info.contexts_arg);
      printf("timelimit: %d\n", args_info.timelimit_arg);
      printf("createif: %d\n", args_info.createif_flag);
      printf("ipup: %s\n", args_info.ipup_arg);
      printf("ipdown: %s\n", args_info.ipdown_arg);
      printf("defaultroute: %d\n", args_info.defaultroute_flag);
      printf("pinghost: %s\n", args_info.pinghost_arg);
      printf("pingrate: %d\n", args_info.pingrate_arg);
      printf("pingsize: %d\n", args_info.pingsize_arg);
      printf("pingcount: %d\n", args_info.pingcount_arg);
      printf("pingquiet: %d\n", args_info.pingquiet_flag);
    }
  }

  /* Handle each option */

  /* foreground                                                   */
  /* If fg flag not given run as a daemon                            */
  if (!args_info.fg_flag)
    {
      closelog(); 
      /* Close the standard file descriptors. Why? */
      freopen("/dev/null", "w", stdout);
      freopen("/dev/null", "w", stderr);
      freopen("/dev/null", "r", stdin);
      daemon(0, 0);
      /* Open log again. This time with new pid */
      openlog(PACKAGE, LOG_PID, LOG_DAEMON);
    }

  /* debug                                                        */
  options.debug = args_info.debug_flag;

  /* pidfile */
  /* This has to be done after we have our final pid */
  if (args_info.pidfile_arg) {
    log_pid(args_info.pidfile_arg);
  }

  /* dns                                                          */
  /* If no dns option is given use system default                 */
  /* Do hostname lookup to translate hostname to IP address       */
  printf("\n");
  if (args_info.dns_arg) {
    if (!(host = gethostbyname(args_info.dns_arg))) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "Invalid DNS address: %s!", args_info.dns_arg);
      return -1;
    }
    else {
      memcpy(&options.dns.s_addr, host->h_addr, host->h_length);
      _res.nscount = 1;
      _res.nsaddr_list[0].sin_addr = options.dns;
      printf("Using DNS server:      %s (%s)\n", 
	     args_info.dns_arg, inet_ntoa(options.dns));
    }
  }
  else {
    options.dns.s_addr= 0;
    printf("Using default DNS server\n");
  }

  /* listen                                                       */
  /* If no listen option is specified listen to any local port    */
  /* Do hostname lookup to translate hostname to IP address       */
  if (args_info.listen_arg) {
    if (!(host = gethostbyname(args_info.listen_arg))) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "Invalid listening address: %s!", args_info.listen_arg);
      return -1;
    }
    else {
      memcpy(&options.listen.s_addr, host->h_addr, host->h_length);
      printf("Local IP address is:   %s (%s)\n", 
	     args_info.listen_arg, inet_ntoa(options.listen));
    }
  }
  else {
    sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	    "Listening address must be specified: %s!", args_info.listen_arg);
    return -1;
  }
  
  
  /* remote                                                       */
  /* If no remote option is specified terminate                   */
  /* Do hostname lookup to translate hostname to IP address       */
  if (args_info.remote_arg) {
    if (!(host = gethostbyname(args_info.remote_arg))) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "Invalid remote address: %s!", args_info.remote_arg);
      return -1;
    }
    else {
      memcpy(&options.remote.s_addr, host->h_addr, host->h_length);
      printf("Remote IP address is:  %s (%s)\n", 
	     args_info.remote_arg, inet_ntoa(options.remote));
    }
  }
  else {
    sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	    "No remote address given!");
    return -1;
  }


  /* imsi                                                            */
  if (strlen(args_info.imsi_arg)!=15) {
    printf("Invalid IMSI\n");
    return -1;
  }
  options.imsi  = ((uint64_t) (args_info.imsi_arg[ 0]-48));
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 1]-48)) <<  4;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 2]-48)) <<  8;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 3]-48)) << 12;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 4]-48)) << 16;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 5]-48)) << 20;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 6]-48)) << 24;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 7]-48)) << 28;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 8]-48)) << 32;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[ 9]-48)) << 36;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[10]-48)) << 40;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[11]-48)) << 44;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[12]-48)) << 48;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[13]-48)) << 52;
  options.imsi |= ((uint64_t) (args_info.imsi_arg[14]-48)) << 56;

    printf("IMSI is:               %s (%#08llx)\n", 
	   args_info.imsi_arg, options.imsi);


  /* qos                                                             */
  options.qos.l = 3;
  options.qos.v[2] = (args_info.qos_arg) & 0xff;
  options.qos.v[1] = ((args_info.qos_arg) >> 8) & 0xff;
  options.qos.v[0] = ((args_info.qos_arg) >> 16) & 0xff;
  
  /* contexts                                                        */
  if (args_info.contexts_arg > MAXCONTEXTS) {
    printf("Contexts has to be less than %d\n", MAXCONTEXTS);
    return -1;
  }
  options.contexts = args_info.contexts_arg;

  /* Timelimit                                                       */
  options.timelimit = args_info.timelimit_arg;
  
  /* apn                                                             */
  if (strlen(args_info.apn_arg) > (sizeof(options.apn.v)-1)) {
    printf("Invalid APN\n");
    return -1;
  }
  options.apn.l = strlen(args_info.apn_arg) + 1;
  options.apn.v[0] = (char) strlen(args_info.apn_arg);
  strncpy(&options.apn.v[1], args_info.apn_arg, sizeof(options.apn.v)-1);
  printf("Using APN:             %s\n", args_info.apn_arg);
  
  /* msisdn                                                          */
  if (strlen(args_info.msisdn_arg)>(sizeof(options.msisdn.v)-1)) {
    printf("Invalid MSISDN\n");
    return -1;
  }
  options.msisdn.l = 1;
  options.msisdn.v[0] = 0x91; /* International format */
  for(n=0; n<strlen(args_info.msisdn_arg); n++) {
    if ((n%2) == 0) {
      options.msisdn.v[((int)n/2)+1] = args_info.msisdn_arg[n] - 48 + 0xf0;
      options.msisdn.l += 1;
    }
    else {
      options.msisdn.v[((int)n/2)+1] = 
	(options.msisdn.v[((int)n/2)+1] & 0x0f) +
	(args_info.msisdn_arg[n] - 48) * 16;
    }
  }
  printf("Using MSISDN:          %s\n", args_info.msisdn_arg);

  /* UID and PWD */
  /* Might need to also insert stuff like DNS etc. */
  if ((strlen(args_info.uid_arg) + strlen(args_info.pwd_arg) + 10)>
      (sizeof(options.pco.v)-1)) {
    printf("invalid UID and PWD\n");
    return -1;
  }
  options.pco.l = strlen(args_info.uid_arg) + strlen(args_info.pwd_arg) + 10;
  options.pco.v[0] = 0x80; /* PPP */
  options.pco.v[1] = 0xc0; /* PAP */
  options.pco.v[2] = 0x23; 
  options.pco.v[3] = 0x12; /* Length of protocol contents */
  options.pco.v[4] = 0x01; /* Authenticate request */
  options.pco.v[5] = 0x01;
  options.pco.v[6] = 0x00; /* MSB of length */
  options.pco.v[7] = strlen(args_info.uid_arg) + strlen(args_info.pwd_arg) + 6;
  options.pco.v[8] = strlen(args_info.uid_arg);
  memcpy(&options.pco.v[9], args_info.uid_arg, strlen(args_info.uid_arg));
  options.pco.v[9+strlen(args_info.uid_arg)] = strlen(args_info.pwd_arg);
  memcpy(&options.pco.v[10+strlen(args_info.uid_arg)], 
	 args_info.pwd_arg, strlen(args_info.pwd_arg));
  
  /* createif */
  options.createif = args_info.createif_flag;

  /* ipup */
  options.ipup = args_info.ipup_arg;

  /* ipdown */
  options.ipdown = args_info.ipdown_arg;

  /* statedir */
  options.statedir = args_info.statedir_arg;

  /* defaultroute */
  options.defaultroute = args_info.defaultroute_flag;


  /* pinghost                                                         */
  /* Store ping host as in_addr                                   */
  if (args_info.pinghost_arg) {
    if (!inet_aton(args_info.pinghost_arg, &options.pinghost)) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "Invalid ping host: %s!", args_info.pinghost_arg);
      return -1;
    }
  }

  /* Other ping parameters                                        */
  options.pingrate = args_info.pingrate_arg;
  options.pingsize = args_info.pingsize_arg;
  options.pingcount = args_info.pingcount_arg;
  options.pingquiet = args_info.pingquiet_flag;

  return 0;

}


int encaps_printf(struct pdp_t *pdp, void *pack, unsigned len) {
  int i;
  printf("The packet looks like this:\n");
  for( i=0; i<len; i++) {
    printf("%02x ", (unsigned char)*(char *)(pack+i));
    if (!((i+1)%16)) printf("\n");
  };
  printf("\n");
  return 0;
}

char * print_ipprot(int t) {
  switch (t) {
  case  1: return "ICMP";
  case  6: return "TCP";
  case 17: return "UDP";
  default: return "Unknown";
  };
}


char * print_icmptype(int t) {
  static char *ttab[] = {
    "Echo Reply",
    "ICMP 1",
    "ICMP 2",
    "Dest Unreachable",
    "Source Quench",
    "Redirect",
    "ICMP 6",
    "ICMP 7",
    "Echo",
    "ICMP 9",
    "ICMP 10",
    "Time Exceeded",
    "Parameter Problem",
    "Timestamp",
    "Timestamp Reply",
    "Info Request",
    "Info Reply"
  };
  if( t < 0 || t > 16 )
    return("OUT-OF-RANGE");  
  return(ttab[t]);
}

/* Calculate time left until we have to send off next ping packet */
int ping_timeout(struct timeval *tp) {
  struct timezone tz;
  struct timeval tv;
  int diff;
  if ((options.pinghost.s_addr) && (2 == state) && 
      ((pingseq < options.pingcount) || (options.pingcount == 0))) {
    gettimeofday(&tv, &tz);
    diff = 1000000 / options.pingrate * pingseq -
      1000000 * (tv.tv_sec - firstping.tv_sec) -
      (tv.tv_usec - firstping.tv_usec); /* Microseconds safe up to 500 sec */
    tp->tv_sec = 0;
    if (diff > 0) 
      tp->tv_usec = diff;
    else {
      /* For some reason we get packet loss if set to zero */
      tp->tv_usec = 100000 / options.pingrate; /* 10 times pingrate */
	 tp->tv_usec = 0; 
    }
  }
  return 0;
}

/* Print out statistics when at the end of ping sequence */
int ping_finish()
{
  struct timezone tz;
  struct timeval tv;
  int elapsed;
  gettimeofday(&tv, &tz);
  elapsed = 1000000 * (tv.tv_sec - firstping.tv_sec) + 
    (tv.tv_usec - firstping.tv_usec); /* Microseconds */
  printf("\n");
  printf("\n----%s PING Statistics----\n", inet_ntoa(options.pinghost));
  printf("%d packets transmitted in %.3f seconds, ", ntransmitted,
	 elapsed / 1000000.0);
  printf("%d packets received, ", nreceived );
  if (ntransmitted) {
    if( nreceived > ntransmitted)
      printf("-- somebody's printing up packets!");
    else
      printf("%d%% packet loss", 
	     (int) (((ntransmitted-nreceived)*100) /
		    ntransmitted));
  }
  printf("\n");
  if (options.debug) printf("%d packets received in total\n", ntreceived );
  if (nreceived  && tsum)
    printf("round-trip (ms)  min/avg/max = %.3f/%.3f/%.3f\n\n",
	   tmin/1000.0,
	   tsum/1000.0/nreceived,
	   tmax/1000.0 );
  printf("%d packets transmitted \n", ntreceived );

  ntransmitted = 0;
  return 0;
}

/* Handle a received ping packet. Print out line and update statistics. */
int encaps_ping(struct pdp_t *pdp, void *pack, unsigned len) {
  struct timezone tz;
  struct timeval tv;
  struct timeval *tp;
  struct ip_ping *pingpack = pack;
  struct in_addr src;
  int triptime;

  src.s_addr = pingpack->src;

  gettimeofday(&tv, &tz);
  if (options.debug) printf("%d.%6d ", (int) tv.tv_sec, (int) tv.tv_usec);

  if (len < CREATEPING_IP + CREATEPING_ICMP) {
    printf("packet too short (%d bytes) from %s\n", len,
	   inet_ntoa(src));
    return 0;
  }

  ntreceived++;
  if (pingpack->protocol != 1) {
    if (!options.pingquiet) printf("%d bytes from %s: ip_protocol=%d (%s)\n",
	   len, inet_ntoa(src), pingpack->protocol, 
	   print_ipprot(pingpack->protocol));
    return 0;
  }

  if (pingpack->type != 0) {
    if (!options.pingquiet) printf("%d bytes from %s: icmp_type=%d (%s) icmp_code=%d\n",
	   len, inet_ntoa(src), pingpack->type, 
	   print_icmptype(pingpack->type), pingpack->code);
    return 0;
  }

  nreceived++;
  if (!options.pingquiet) printf("%d bytes from %s: icmp_seq=%d", len,
	 inet_ntoa(src), ntohs(pingpack->seq));

  if (len >= sizeof(struct timeval) + CREATEPING_IP + CREATEPING_ICMP) {
    gettimeofday(&tv, &tz);
    tp = (struct timeval *) pingpack->data;
    if( (tv.tv_usec -= tp->tv_usec) < 0 )   {
      tv.tv_sec--;
      tv.tv_usec += 1000000;
    }
    tv.tv_sec -= tp->tv_sec;

    triptime = tv.tv_sec*1000000+(tv.tv_usec);
    tsum += triptime;
    if( triptime < tmin )
      tmin = triptime;
    if( triptime > tmax )
      tmax = triptime;

    if (!options.pingquiet) printf(" time=%.3f ms\n", triptime/1000.0);

  } 
  else
    if (!options.pingquiet) printf("\n");
  return 0;
}

/* Create a new ping packet and send it off to peer. */
int create_ping(void *gsn, struct pdp_t *pdp,
		struct in_addr *dst, int seq, int datasize) {

  struct ip_ping pack;
  u_int16_t *p = (u_int16_t *) &pack;
  u_int8_t  *p8 = (u_int8_t *) &pack;
  struct in_addr src;
  int n;
  long int sum = 0;
  int count = 0;

  struct timezone tz;
  struct timeval *tp = (struct timeval *) &p8[CREATEPING_IP + CREATEPING_ICMP];

  if (datasize > CREATEPING_MAX) {
    sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	    "Ping size to large: %d!", datasize);
    return -1;
  }

  memcpy(&src, &(pdp->eua.v[2]), 4); /* Copy a 4 byte address */

  pack.ipver  = 0x45;
  pack.tos    = 0x00;
  pack.length = htons(CREATEPING_IP + CREATEPING_ICMP + datasize);
  pack.fragid = 0x0000;
  pack.offset = 0x0040;
  pack.ttl    = 0x40;
  pack.protocol = 0x01;
  pack.ipcheck = 0x0000;
  pack.src = src.s_addr;
  pack.dst = dst->s_addr;
  pack.type = 0x08;
  pack.code = 0x00;
  pack.checksum = 0x0000;
  pack.ident = 0x0000;
  pack.seq = htons(seq);

  /* Generate ICMP payload */
  p8 = (u_int8_t *) &pack + CREATEPING_IP + CREATEPING_ICMP;
  for (n=0; n<(datasize); n++) p8[n] = n;

  if (datasize >= sizeof(struct timeval)) 
    gettimeofday(tp, &tz);

  /* Calculate IP header checksum */
  p = (u_int16_t *) &pack;
  count = CREATEPING_IP;
  sum = 0;
  while (count>1) {
    sum += *p++;
    count -= 2;
  }
  while (sum>>16) 
    sum = (sum & 0xffff) + (sum >> 16);
  pack.ipcheck = ~sum;


  /* Calculate ICMP checksum */
  count = CREATEPING_ICMP + datasize; /* Length of ICMP message */
  sum = 0;
  p = (u_int16_t *) &pack;
  p += CREATEPING_IP / 2;
  while (count>1) {
    sum += *p++;
    count -= 2;
  }
  if (count>0)
    sum += * (unsigned char *) p;
  while (sum>>16) 
    sum = (sum & 0xffff) + (sum >> 16);
  pack.checksum = ~sum;

  ntransmitted++;
  return gtp_gpdu(gsn, pdp, &pack, 28 + datasize);
}
		

int delete_context(struct pdp_t *pdp) {

  if (tun && options.ipdown) tun_runscript(tun, options.ipdown);

  ipdel((struct iphash_t*) pdp->peer);
  memset(pdp->peer, 0, sizeof(struct iphash_t)); /* To be sure */
  return 0;
}


/* Callback for receiving messages from tun */
int cb_tun_ind(struct tun_t *tun, void *pack, unsigned len) {
  struct iphash_t *ipm;
  struct in_addr src;
  struct tun_packet_t *iph = (struct tun_packet_t*) pack;

  src.s_addr = iph->src;

  if (ipget(&ipm, &src)) {
    printf("Received packet without a valid source address!!!\n");
    return 0;
  }
  
  if (ipm->pdp) /* Check if a peer protocol is defined */
    gtp_gpdu(gsn, ipm->pdp, pack, len);
  return 0;
}

int create_pdp_conf(struct pdp_t *pdp, int cause) {
  struct in_addr addr;

  if (cause != 128) {
    printf("Received create PDP context response. Cause value: %d\n", cause);
    state = 0;
    return EOF; /* Not what we expected */
  }

  if (pdp_euaton(&pdp->eua, &addr)) {
    printf("Received create PDP context response. Cause value: %d\n", cause);
    state = 0;
    return EOF; /* Not a valid IP address */
  }

  printf("Received create PDP context response. IP address: %s\n", 
	 inet_ntoa(addr));

  if (options.createif) {
    struct in_addr m;
    inet_aton("255.255.255.255", &m);
    /* printf("Setting up interface and routing\n");*/
    tun_addaddr(tun, &addr,  &addr, &m);
    if (options.defaultroute) {
      struct in_addr rm;
      rm.s_addr = 0;
      tun_addroute(tun, &rm,  &addr, &rm);
    }
    if (options.ipup) tun_runscript(tun, options.ipup);
  }
    
  ipset((struct iphash_t*) pdp->peer, &addr);
  
  state = 2;                      /* Connected */

  return 0;
}

int delete_pdp_conf(struct pdp_t *pdp, int cause) {
  printf("Received delete PDP context response. Cause value: %d\n", cause);
  state = 0; /* Idle */
  return 0;
}

int echo_conf(struct pdp_t *pdp, int cause) {
  if (cause <0)
    printf("Echo request timed out\n");
  else
    printf("Received echo response.\n");
  return 0;
}

int conf(int type, int cause, struct pdp_t* pdp, void *aid) {
  /* if (cause < 0) return 0; Some error occurred. We don't care */
  switch (type) {
  case GTP_ECHO_REQ:
    return echo_conf(pdp, cause);
  case GTP_CREATE_PDP_REQ:
    if (cause !=128) return 0; /* Request not accepted. We don't care */
    return create_pdp_conf(pdp, cause);
  case GTP_DELETE_PDP_REQ:
    if (cause !=128) return 0; /* Request not accepted. We don't care */
    return delete_pdp_conf(pdp, cause);
  default:
    return 0;
  }
}


int encaps_tun(struct pdp_t *pdp, void *pack, unsigned len) {
  /*  printf("encaps_tun. Packet received: forwarding to tun\n");*/
  return tun_encaps((struct tun_t*) pdp->ipif, pack, len);
}

int main(int argc, char **argv)
{
  fd_set fds;			/* For select() */
  struct timeval idleTime;	/* How long to select() */
  struct pdp_t *pdp;
  int n;
  int starttime = time(NULL);   /* Time program was started */

  struct timezone tz;           /* Used for calculating ping times */
  struct timeval tv;
  int diff;

  /* open a connection to the syslog daemon */
  /*openlog(PACKAGE, LOG_PID, LOG_DAEMON);*/
  openlog(PACKAGE, (LOG_PID | LOG_PERROR), LOG_DAEMON);

  /* Process options given in configuration file and command line */
  if (process_options(argc, argv)) 
    exit(1);

  printf("\nInitialising GTP library\n");
  if (gtp_new(&gsn, options.statedir,  &options.listen)) {
    sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	    "Failed to create gtp");
    exit(1);
  }
  if (gsn->fd > maxfd) maxfd = gsn->fd;

  gtp_set_cb_delete_context(gsn, delete_context);
  gtp_set_cb_conf(gsn, conf);
  if (options.createif) 
    gtp_set_cb_gpdu(gsn, encaps_tun);
  else
    gtp_set_cb_gpdu(gsn, encaps_ping);

  if (options.createif) {
    printf("Setting up interface\n");
    /* Create a tunnel interface */
    if (tun_new((struct tun_t**) &tun)) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "Failed to create tun");
      exit(1);
    }
    tun_set_cb_ind(tun, cb_tun_ind);
    if (tun->fd > maxfd) maxfd = tun->fd;
  }

  /* Initialise hash tables */
  memset(&iphash, 0, sizeof(iphash));  
  memset(&iparr, 0, sizeof(iparr));  

  printf("Done initialising GTP library\n\n");

  /* See if anybody is there */
  printf("Sending off echo request\n");
  gtp_echo_req(gsn, &options.remote); /* See if remote is alive ? */

  for(n=0; n<options.contexts; n++) {
    printf("Setting up PDP context #%d\n", n);
    iparr[n].inuse = 1; /* TODO */

    /* Allocated here. Cleaned up in gtp.c:*/
    pdp_newpdp(&pdp, options.imsi, n, NULL); 

    pdp->peer = &iparr[n];
    pdp->ipif = tun; /* TODO */
    iparr[n].pdp = pdp;

    if (options.qos.l > sizeof(pdp->qos_req0)) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0, "QoS length too big");
      exit(1);
    }
    else {
      memcpy(pdp->qos_req0, options.qos.v, options.qos.l);
    }
    
    pdp->selmode = 0x01; /* MS provided APN, subscription not verified */
    
    if (options.apn.l > sizeof(pdp->apn_use.v)) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0, "APN length too big");
      exit(1);
    }
    else {
      pdp->apn_use.l = options.apn.l;
      memcpy(pdp->apn_use.v, options.apn.v, options.apn.l);
    }
    
    pdp->gsnlc.l = sizeof(options.listen);
    memcpy(pdp->gsnlc.v, &options.listen, sizeof(options.listen));
    pdp->gsnlu.l = sizeof(options.listen);
    memcpy(pdp->gsnlu.v, &options.listen, sizeof(options.listen));
    
    if (options.msisdn.l > sizeof(pdp->msisdn.v)) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0, "MSISDN length too big");
      exit(1);
    }
    else {
      pdp->msisdn.l = options.msisdn.l;
      memcpy(pdp->msisdn.v, options.msisdn.v, options.msisdn.l);
    }
    
    ipv42eua(&pdp->eua, NULL); /* Request dynamic IP address */
    
    if (options.pco.l > sizeof(pdp->pco_req.v)) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0, "PCO length too big");
      exit(1);
    }
    else {
      pdp->pco_req.l = options.pco.l;
      memcpy(pdp->pco_req.v, options.pco.v, options.pco.l);
    }
    
    /* Create context */
    /* We send this of once. Retransmissions are handled by gtplib */
    gtp_create_context(gsn, pdp, NULL, &options.remote);
  }    

  state = 1;  /* Enter wait_connection state */

  printf("Waiting for response from ggsn........\n\n");


  /******************************************************************/
  /* Main select loop                                               */
  /******************************************************************/

  while ((((starttime + options.timelimit + 10) > time(NULL)) 
	 || (0 == options.timelimit)) && (state!=0)) {

    /* Take down client connections at some stage */
    if (((starttime + options.timelimit) <= time(NULL)) &&
	(0 != options.timelimit) && (2 == state)) {
      state = 3;
      for(n=0; n<options.contexts; n++) {
	/* Delete context */
	printf("Disconnecting PDP context #%d\n", n);
	gtp_delete_context(gsn, iparr[n].pdp, NULL);
	if ((options.pinghost.s_addr !=0) && ntransmitted) ping_finish();
      }
    }
    
    diff = 0;
    while (( diff<=0 ) && 
    /* Send off an ICMP ping packet */
	   /*if (*/(options.pinghost.s_addr) && (2 == state) && 
	((pingseq < options.pingcount) || (options.pingcount == 0))) {
      if (!pingseq) gettimeofday(&firstping, &tz); /* Set time of first ping */
      gettimeofday(&tv, &tz);
      diff = 1000000 / options.pingrate * pingseq -
	1000000 * (tv.tv_sec - firstping.tv_sec) -
	(tv.tv_usec - firstping.tv_usec); /* Microseconds safe up to 500 sec */
      if (diff <=0) {
	if (options.debug) printf("Create_ping %d\n", diff);
	create_ping(gsn, iparr[pingseq % options.contexts].pdp,
		    &options.pinghost, pingseq, options.pingsize);
	pingseq++;
      }
    }
    

    if (ntransmitted && options.pingcount && nreceived >= options.pingcount)
      ping_finish();
    

    FD_ZERO(&fds);
    if (tun) FD_SET(tun->fd, &fds);
    FD_SET(gsn->fd, &fds);
    
    gtp_retranstimeout(gsn, &idleTime);
    ping_timeout(&idleTime);
    
    if (options.debug) printf("idletime.tv_sec %d, idleTime.tv_usec %d\n",
			      (int) idleTime.tv_sec, (int) idleTime.tv_usec);
    
    switch (select(maxfd + 1, &fds, NULL, NULL, &idleTime)) {
    case -1:
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "Select returned -1");
      break;  
    case 0:
      gtp_retrans(gsn); /* Only retransmit if nothing else */
      break; 
    default:
      break;
    }
    
    if ((tun) && FD_ISSET(tun->fd, &fds) && tun_decaps(tun) < 0) {
      sys_err(LOG_ERR, __FILE__, __LINE__, 0,
	      "TUN decaps failed");
    }
    
    if (FD_ISSET(gsn->fd, &fds))
      gtp_decaps(gsn);
    
  }
  
  gtp_free(gsn); /* Clean up the gsn instance */
  
  if (options.createif)
    tun_free(tun);
  
  return 0; 
}

