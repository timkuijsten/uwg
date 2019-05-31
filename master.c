/*
 * Copyright (c) 2018, 2019 Tim Kuijsten
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/socket.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <limits.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include "util.h"
#include "wireprot.h"

#include "parseconfig.h"

#define DEFAULTCONFIG "/etc/wiresep.conf"

typedef int chan[2];

void enclave_init(int masterport);
void proxy_init(int masterport);
void ifn_init(int masterport);
void enclave_serv(void);
void proxy_serv(void);
void ifn_serv(void);

void master_printinfo(FILE *);

/* these are used by the other modules as well */
int background, verbose;

/* global settings */
static char *configfile;
static uid_t guid;
static gid_t ggid;

static const wskey nullkey;

static struct ifn **ifnv;
static size_t ifnvsize;

static union smsg smsg;

/* communication channels */
int mastwithencl, enclwithmast, mastwithprox, proxwithmast, enclwithprox,
    proxwithencl;

/*
 * Send interface info to the enclave.
 *
 * SINIT
 * SIFN
 * SPEER
 *
 * Exit on error.
 */
void
sendconfig_enclave(int mast2encl, int enclwithprox)
{
	struct ifn *ifn;
	struct peer *peer;
	size_t n, m;

	memset(&smsg.init, 0, sizeof(smsg.init));

	smsg.init.background = background;
	smsg.init.verbose = verbose;
	smsg.init.uid = guid;
	smsg.init.gid = ggid;
	smsg.init.proxport = enclwithprox;
	smsg.init.nifns = ifnvsize;

	if (wire_sendmsg(mast2encl, SINIT, &smsg.init, sizeof(smsg.init)) == -1)
		logexitx(1, "%s wire_sendmsg SINIT", __func__);

	for (n = 0; n < ifnvsize; n++) {
		ifn = ifnv[n];

		memset(&smsg.ifn, 0, sizeof(smsg.ifn));

		smsg.ifn.ifnid = n;
		smsg.ifn.ifnport = ifn->enclwithifn;
		snprintf(smsg.ifn.ifname, sizeof(smsg.ifn.ifname), "%s", ifn->ifname);
		if (ifn->ifdesc && strlen(ifn->ifdesc) > 0)
			snprintf(smsg.ifn.ifdesc, sizeof(smsg.ifn.ifdesc), "%s", ifn->ifdesc);
		memcpy(smsg.ifn.privkey, ifn->privkey, sizeof(smsg.ifn.privkey));
		memcpy(smsg.ifn.pubkey, ifn->pubkey, sizeof(smsg.ifn.pubkey));
		memcpy(smsg.ifn.pubkeyhash, ifn->pubkeyhash, sizeof(smsg.ifn.pubkeyhash));
		memcpy(smsg.ifn.mac1key, ifn->mac1key, sizeof(smsg.ifn.mac1key));
		memcpy(smsg.ifn.cookiekey, ifn->cookiekey, sizeof(smsg.ifn.cookiekey));
		smsg.ifn.npeers = ifn->peerssize;

		if (wire_sendmsg(mast2encl, SIFN, &smsg.ifn, sizeof(smsg.ifn)) == -1)
			logexitx(1, "%s wire_sendmsg SIFN", __func__);

		for (m = 0; m < ifn->peerssize; m++) {
			peer = ifn->peers[m];

			memset(&smsg.peer, 0, sizeof(smsg.peer));

			smsg.peer.ifnid = n;
			smsg.peer.peerid = m;

			if (memcmp(peer->psk, nullkey, sizeof(wskey)) == 0)
				memcpy(peer->psk, ifn->psk, sizeof(wskey));

			memcpy(smsg.peer.psk, peer->psk, sizeof(smsg.peer.psk));
			memcpy(smsg.peer.peerkey, peer->pubkey,
			    sizeof(smsg.peer.peerkey));
			memcpy(smsg.peer.mac1key, peer->mac1key,
			    sizeof(smsg.peer.mac1key));

			if (wire_sendmsg(mast2encl, SPEER, &smsg.peer,
			    sizeof(smsg.peer)) == -1)
				logexitx(1, "%s wire_sendmsg SPEER", __func__);
		}
	}

	/* wait with end of startup signal */

	explicit_bzero(&smsg, sizeof(smsg));

	loginfox("config sent to enclave %d", mast2encl);
}

/*
 * Send interface info to the proxy.
 *
 * "mast2prox" is used to send the config from this process to the proxy
 * process.
 * "proxwithencl" is the descriptor the proxy process must use to communicate
 * with the enclave.
 *
 * The descriptors the proxy process must use to communicate with
 * each ifn process are in each ifn structure.
 *
 * SINIT
 * SIFN
 *
 * Exit on error.
 */
void
sendconfig_proxy(int mast2prox, int proxwithencl)
{
	struct ifn *ifn;
	struct sockaddr_storage *listenaddr;
	size_t m, n;

	memset(&smsg.init, 0, sizeof(smsg.init));

	smsg.init.background = background;
	smsg.init.verbose = verbose;
	smsg.init.uid = guid;
	smsg.init.gid = ggid;
	smsg.init.enclport = proxwithencl;
	smsg.init.nifns = ifnvsize;

	if (wire_sendmsg(mast2prox, SINIT, &smsg.init, sizeof(smsg.init)) == -1)
		logexitx(1, "%s wire_sendmsg SINIT", __func__);

	for (n = 0; n < ifnvsize; n++) {
		ifn = ifnv[n];

		memset(&smsg.ifn, 0, sizeof(smsg.ifn));

		smsg.ifn.ifnid = n;
		smsg.ifn.ifnport = ifn->proxport;
		smsg.ifn.nlistenaddrs = ifn->listenaddrssize;
		snprintf(smsg.ifn.ifname, sizeof(smsg.ifn.ifname), "%s",
		    ifn->ifname);
		/* don't send interface description to proxy, no public keys in
		 * the proxy process has small benefits because they're
		 * semi-trusted in wireguard.
		 */
		memcpy(smsg.ifn.mac1key, ifn->mac1key,
		    sizeof(smsg.ifn.mac1key));
		memcpy(smsg.ifn.cookiekey, ifn->cookiekey,
		    sizeof(smsg.ifn.cookiekey));
		smsg.ifn.npeers = ifn->peerssize;

		if (wire_sendmsg(mast2prox, SIFN, &smsg.ifn, sizeof(smsg.ifn))
		    == -1)
			logexitx(1, "%s wire_sendmsg SIFN", __func__);

		/* send listen addresses */
		for (m = 0; m < ifn->listenaddrssize; m++) {
			listenaddr = ifn->listenaddrs[m];

			memset(&smsg.cidraddr, 0, sizeof(smsg.cidraddr));

			smsg.cidraddr.ifnid = n;
			memcpy(&smsg.cidraddr.addr, listenaddr,
			    sizeof(smsg.cidraddr.addr));

			if (wire_sendmsg(mast2prox, SCIDRADDR, &smsg.cidraddr,
			    sizeof(smsg.cidraddr)) == -1)
				logexitx(1, "%s wire_sendmsg SCIDRADDR", __func__);
		}
	}

	/* wait with end of startup signal */

	explicit_bzero(&smsg, sizeof(smsg));

	loginfox("config sent to proxy %d", mast2prox);
}

/*
 * Send interface info to an ifn process.
 *
 * SINIT
 * SIFN
 * SPEER
 * SCIDRADDR
 *
 * Exit on error.
 */
void
sendconfig_ifn(int ifnid)
{
	struct cidraddr *allowedip;
	struct cidraddr *ifaddr;
	struct sockaddr_storage *listenaddr;
	struct ifn *ifn;
	struct peer *peer;
	size_t m, n;

	if (ifnid < 0)
		logexitx(1, "%s", __func__);
	if ((size_t)ifnid >= ifnvsize)
		logexitx(1, "%s", __func__);

	ifn = ifnv[ifnid];

	memset(&smsg.init, 0, sizeof(smsg.init));

	smsg.init.background = background;
	smsg.init.verbose = verbose;
	smsg.init.uid = ifn->uid;
	smsg.init.gid = ifn->gid;
	smsg.init.enclport = ifn->ifnwithencl;
	smsg.init.proxport = ifn->ifnwithprox;

	if (wire_sendmsg(ifn->mastwithifn, SINIT, &smsg.init, sizeof(smsg.init)) == -1)
		logexitx(1, "%s wire_sendmsg SINIT %d", __func__, ifn->mastwithifn);

	memset(&smsg.ifn, 0, sizeof(smsg.ifn));

	smsg.ifn.ifnid = ifnid;
	snprintf(smsg.ifn.ifname, sizeof(smsg.ifn.ifname), "%s", ifn->ifname);
	if (ifn->ifdesc && strlen(ifn->ifdesc) > 0)
		snprintf(smsg.ifn.ifdesc, sizeof(smsg.ifn.ifdesc), "%s", ifn->ifdesc);
	memcpy(smsg.ifn.mac1key, ifn->mac1key, sizeof(smsg.ifn.mac1key));
	memcpy(smsg.ifn.cookiekey, ifn->cookiekey, sizeof(smsg.ifn.cookiekey));
	smsg.ifn.nifaddrs = ifn->ifaddrssize;
	smsg.ifn.nlistenaddrs = ifn->listenaddrssize;
	smsg.ifn.npeers = ifn->peerssize;

	if (wire_sendmsg(ifn->mastwithifn, SIFN, &smsg.ifn, sizeof(smsg.ifn)) == -1)
		logexitx(1, "%s wire_sendmsg SIFN %s", __func__, ifn->ifname);

	/* first send interface addresses */
	for (n = 0; n < ifn->ifaddrssize; n++) {
		ifaddr = ifn->ifaddrs[n];

		memset(&smsg.cidraddr, 0, sizeof(smsg.cidraddr));

		smsg.cidraddr.ifnid = ifnid;
		smsg.cidraddr.prefixlen = ifaddr->prefixlen;
		memcpy(&smsg.cidraddr.addr, &ifaddr->addr,
		    sizeof(smsg.cidraddr.addr));

		if (wire_sendmsg(ifn->mastwithifn, SCIDRADDR, &smsg.cidraddr,
		    sizeof(smsg.cidraddr)) == -1)
			logexitx(1, "%s wire_sendmsg SCIDRADDR", __func__);
	}

	/* then listen addresses */
	for (n = 0; n < ifn->listenaddrssize; n++) {
		listenaddr = ifn->listenaddrs[n];

		memset(&smsg.cidraddr, 0, sizeof(smsg.cidraddr));

		smsg.cidraddr.ifnid = ifnid;
		memcpy(&smsg.cidraddr.addr, listenaddr,
		    sizeof(smsg.cidraddr.addr));

		if (wire_sendmsg(ifn->mastwithifn, SCIDRADDR, &smsg.cidraddr,
		    sizeof(smsg.cidraddr)) == -1)
			logexitx(1, "%s wire_sendmsg SCIDRADDR", __func__);
	}

	/* at last send the peers */
	for (m = 0; m < ifn->peerssize; m++) {
		peer = ifn->peers[m];

		memset(&smsg.peer, 0, sizeof(smsg.peer));

		smsg.peer.ifnid = ifnid;
		smsg.peer.peerid = m;
		snprintf(smsg.peer.name, sizeof(smsg.peer.name), "%s", peer->name);
		smsg.peer.nallowedips = peer->allowedipssize;
		memcpy(&smsg.peer.fsa, &peer->fsa, sizeof(smsg.peer.fsa));

		if (wire_sendmsg(ifn->mastwithifn, SPEER, &smsg.peer, sizeof(smsg.peer))
		    == -1)
			logexitx(1, "wire_sendmsg SPEER %zu", m);

		for (n = 0; n < peer->allowedipssize; n++) {
			allowedip = peer->allowedips[n];

			memset(&smsg.cidraddr, 0, sizeof(smsg.cidraddr));

			smsg.cidraddr.ifnid = ifnid;
			smsg.cidraddr.peerid = m;
			smsg.cidraddr.prefixlen = allowedip->prefixlen;
			memcpy(&smsg.cidraddr.addr, &allowedip->addr,
			    sizeof(smsg.cidraddr.addr));

			if (wire_sendmsg(ifn->mastwithifn, SCIDRADDR, &smsg.cidraddr,
			    sizeof(smsg.cidraddr)) == -1)
				logexitx(1, "wire_sendmsg SCIDRADDR");
		}
	}

	/* wait with end of startup signal */

	explicit_bzero(&smsg, sizeof(smsg));

	loginfox("config sent to %s %d", ifn->ifname, ifn->mastwithifn);
}

/*
 * Signal end of configuration.
 */
void
signal_eos(int mastport)
{
	memset(&smsg, 0, sizeof(smsg));

	if (wire_sendmsg(mastport, SEOS, &smsg.eos, sizeof(smsg.eos)) == -1)
		logexitx(1, "%s wire_sendmsg SEOS %d", __func__, mastport);
}

void
printdescriptors(void)
{
	size_t n;

	loginfox("enclave %d:%d", mastwithencl, enclwithmast);
	loginfox("proxy %d:%d", mastwithprox, proxwithmast);

	for (n = 0; n < ifnvsize; n++) {
		loginfox("%s master %d:%d, enclave %d:%d, proxy %d:%d", ifnv[n]->ifname,
		    ifnv[n]->mastwithifn,
		    ifnv[n]->ifnwithmast,
		    ifnv[n]->enclwithifn,
		    ifnv[n]->ifnwithencl,
		    ifnv[n]->proxwithifn,
		    ifnv[n]->ifnwithprox);
	}
}

void
printusage(FILE *fp)
{
	fprintf(fp, "usage: %s [-dnqv] [-f file]\n", getprogname());
}

/*
 * Bootstrap the application:
 *   0. read configuration
 *   1. determine public key, mac1key and cookie key of each interface
 *   2. setup communication ports and fork each IFN, the PROXY and the ENCLAVE
 *   3. send startup info to processes
 *   4. reexec and idle
 */
int
main(int argc, char **argv)
{
	/* descriptors for all communication channels */
	chan *ifchan, mastmast, tmpchan;
	size_t n, m;
	int configtest, foreground, stdopen, masterport, stat;
	pid_t pid;
	const char *errstr;
	char c, *eargs[4], *eenv[1], *logfacilitystr, *oldprogname;

	/* should endup in a configure script */
	if (sizeof(struct msgwginit) != 148)
		errx(1, "sizeof(struct msgwginit != 148: %zu",
		    sizeof(struct msgwginit));

	if (sizeof(struct msgwgresp) != 92)
		errx(1, "sizeof(struct msgwgresp) != 92: %zu",
		    sizeof(struct msgwgresp));

	if (sizeof(struct msgwgcook) != 64)
		errx(1, "sizeof(struct msgwgcook) != 64: %zu",
		    sizeof(struct msgwgcook));

	if (sizeof(struct msgwgdatahdr) != 16)
		errx(1, "sizeof(struct msgwgdatahdr) != 16: %zu",
		    sizeof(struct msgwgdatahdr));

	configtest = 0;
	foreground = 0;
	while ((c = getopt(argc, argv, "E:I:M:P:df:hnqv")) != -1)
		switch(c) {
		case 'E':
			masterport = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "invalid enclave/master fd: %s %s",
				    errstr, optarg);
			setproctitle(NULL);
			enclave_init(masterport);
			enclave_serv();
			errx(1, "enclave[%d]: unexpected return", getpid());
		case 'I':
			masterport = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "invalid ifn/master fd: %s %s",
				    errstr, optarg);
			setproctitle(NULL);
			ifn_init(masterport);
			ifn_serv();
			errx(1, "ifn[%d]: unexpected return", getpid());
		case 'P':
			masterport = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "invalid proxy/master fd: %s %s",
				    errstr, optarg);
			setproctitle(NULL);
			proxy_init(masterport);
			proxy_serv();
			errx(1, "proxy[%d]: unexpected return", getpid());
		case 'M':
			mastmast[1] = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "invalid mastermaster descriptor: %s "
				    "%s", errstr, optarg);
			setproctitle(NULL);

			if (chroot(EMPTYDIR) == -1 || chdir("/") == -1)
				err(1, "chroot %s", EMPTYDIR);
			if (pledge("stdio", "") == -1)
				err(1, "%s: pledge", __func__);

			if (read(mastmast[1], &mastwithencl, sizeof(int))
			    != sizeof(int))
				err(1, "could not read enclave descriptor in "
				    "new master");
			if (read(mastmast[1], &mastwithprox, sizeof(int))
			    != sizeof(int))
				err(1, "could not read proxy descriptor in new "
				    "master");
			if (read(mastmast[1], &ifnvsize, sizeof(ifnvsize))
			    != sizeof(ifnvsize))
				err(1, "could not read ifnvsize in new master");
			if ((ifchan = calloc(ifnvsize, sizeof(*ifchan)))
			    == NULL)
				err(1, "calloc ifchan");
			for (n = 0; n < ifnvsize; n++) {
				if (read(mastmast[1], &ifchan[n][0],
				    sizeof(int)) != sizeof(int))
					err(1, "could not read ifn descriptor "
					    "in new master");
			}
			close(mastmast[1]);

			/*
			 * Signal that we are ready and each process may proceed
			 * and start processing untrusted input.
			 */
			signal_eos(mastwithencl);
			signal_eos(mastwithprox);
			for (n = 0; n < ifnvsize; n++)
				signal_eos(ifchan[n][0]);

			if ((pid = waitpid(WAIT_ANY, &stat, 0)) == -1)
				err(1, "waitpid");

			if (WIFEXITED(stat)) {
				warnx("child %d normal exit %d", pid,
				    WEXITSTATUS(stat));
			} else if (WIFSIGNALED(stat)) {
				warnx("child %d exit by signal %d %s%s", pid,
				    WTERMSIG(stat), strsignal(WTERMSIG(stat)),
				    WCOREDUMP(stat) ? " (core)" : "");
			} else
				warnx("unknown termination status");

			if (killpg(0, SIGTERM) == -1)
				err(1, "killpg");

			/* should never reach */
			exit(3);
		case 'd':
			foreground = 1;
			break;
		case 'f':
			configfile = optarg;
			break;
		case 'h':
			printusage(stdout);
			exit(0);
		case 'n':
			configtest = 1;
			break;
		case 'q':
			verbose--;
			break;
		case 'v':
			verbose++;
			break;
		case '?':
			printusage(stderr);
			exit(1);
		}

	argc -= optind;
	argv += optind;

	if (argc != 0) {
		printusage(stderr);
		exit(1);
	}

	if (pledge("stdio dns rpath proc exec getpw", NULL) == -1)
		err(1, "%s: pledge", __func__);

	if (geteuid() != 0)
		errx(1, "must run as the superuser");

	/*
	 *   0. read configuration
	 */

	if (configfile) {
		xparseconfigfile(configfile, &ifnv, &ifnvsize, &guid, &ggid,
		    &logfacilitystr);
	} else {
		xparseconfigfile(DEFAULTCONFIG, &ifnv, &ifnvsize, &guid, &ggid,
		    &logfacilitystr);
	}

	if (configtest)
		exit(0);

	if (!foreground) {
		background = 1;
		if (daemonize() == -1)
			err(1, "daemonize"); /* might not print to stdout */
	}

	if (initlog(logfacilitystr) == -1)
		logexitx(1, "could not init log"); /* not printed if daemon */

	/*
	 *   1. determine public key, mac1key and cookie key of each interface
	 */

	processconfig();

	stdopen = isopenfd(STDIN_FILENO) + isopenfd(STDOUT_FILENO) +
	    isopenfd(STDERR_FILENO);

	assert(getdtablecount() == stdopen);

	/*
	 *   2. setup communication ports and fork each IFN, the PROXY and the
	 *     ENCLAVE
	 */

	/* don't bother to free before exec */
	if ((oldprogname = strdup(getprogname())) == NULL)
		logexit(1, "strdup getprogname");

	eenv[0] = NULL;

	for (n = 0; n < ifnvsize; n++) {
		/*
		 * Open an interface channel with master, enclave and proxy, respectively
		 */

		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tmpchan) == -1)
			logexit(1, "socketpair ifnmast %zu", n);

		ifnv[n]->mastwithifn = tmpchan[0];
		ifnv[n]->ifnwithmast = tmpchan[1];

		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tmpchan) == -1)
			logexit(1, "socketpair ifnencl %zu", n);

		ifnv[n]->enclwithifn = tmpchan[0];
		ifnv[n]->ifnwithencl = tmpchan[1];

		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tmpchan) == -1)
			logexit(1, "socketpair ifnprox %zu", n);

		ifnv[n]->proxwithifn = tmpchan[0];
		ifnv[n]->ifnwithprox = tmpchan[1];

		switch (fork()) {
		case -1:
			logexit(1, "fork %s", ifnv[n]->ifname);
		case 0:
			setprogname(ifnv[n]->ifname);
			if (verbose > 1)
				loginfox("%d", getpid());

			for (m = 0; m <= n; m++) {
				close(ifnv[n]->mastwithifn);
				close(ifnv[n]->enclwithifn);
				close(ifnv[n]->proxwithifn);
			}

			assert(getdtablecount() == stdopen + 3);

			eargs[0] = (char *)getprogname();
			eargs[1] = "-I";
			if (asprintf(&eargs[2], "%u", ifnv[n]->ifnwithmast) < 1)
				logexitx(1, "asprintf");
			/* don't bother to free before exec */
			eargs[3] = NULL;
			execvpe(oldprogname, eargs, eenv);
			logexit(1, "exec ifn");
		}

		/* parent */
		close(ifnv[n]->ifnwithmast);
		close(ifnv[n]->ifnwithencl);
		close(ifnv[n]->ifnwithprox);

		assert(getdtablecount() == stdopen + (int)(n + 1) * 3);
	}

	/*
	 * Setup channels between master, proxy and enclave.
	 */

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tmpchan) == -1)
		logexit(1, "socketpair");

	mastwithencl = tmpchan[0];
	enclwithmast = tmpchan[1];

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tmpchan) == -1)
		logexit(1, "socketpair");

	mastwithprox = tmpchan[0];
	proxwithmast = tmpchan[1];

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, tmpchan) == -1)
		logexit(1, "socketpair");

	enclwithprox = tmpchan[0];
	proxwithencl = tmpchan[1];

	assert(getdtablecount() == stdopen + 6 + (int)ifnvsize * 3);

	/* fork enclave */
	switch (fork()) {
	case -1:
		logexit(1, "fork enclave");
	case 0:
		setprogname("enclave");
		if (verbose > 1)
			loginfox("%d", getpid());

		for (n = 0; n < ifnvsize; n++) {
			close(ifnv[n]->mastwithifn);
			close(ifnv[n]->proxwithifn);
		}

		close(mastwithprox);
		close(mastwithencl);
		close(proxwithmast);
		close(proxwithencl);

		assert(getdtablecount() == stdopen + 2 + (int)ifnvsize);

		eargs[0] = (char *)getprogname();
		eargs[1] = "-E";
		if (asprintf(&eargs[2], "%d", enclwithmast) < 1)
			logexitx(1, "asprintf");
		/* don't bother to free before exec */
		eargs[3] = NULL;
		execvpe(oldprogname, eargs, eenv);
		logexit(1, "exec enclave");
	}

	close(enclwithmast);
	close(enclwithprox);

	for (n = 0; n < ifnvsize; n++)
		close(ifnv[n]->enclwithifn);

	assert(getdtablecount() == stdopen + 4 + (int)ifnvsize * 2);

	/* fork proxy  */
	switch (fork()) {
	case -1:
		logexit(1, "fork proxy");
	case 0:
		setprogname("proxy");
		if (verbose > 1)
			loginfox("%d", getpid());

		for (n = 0; n < ifnvsize; n++)
			close(ifnv[n]->mastwithifn);

		close(mastwithencl);
		close(mastwithprox);

		assert(getdtablecount() == stdopen + 2 + (int)ifnvsize);

		eargs[0] = (char *)getprogname();
		eargs[1] = "-P";
		if (asprintf(&eargs[2], "%d", proxwithmast) < 1)
			logexitx(1, "asprintf");
		/* don't bother to free before exec */
		eargs[3] = NULL;
		execvpe(oldprogname, eargs, eenv);
		logexit(1, "exec proxy");
	}

	close(proxwithmast);
	close(proxwithencl);

	for (n = 0; n < ifnvsize; n++)
		close(ifnv[n]->proxwithifn);

	assert(getdtablecount() == stdopen + 2 + (int)ifnvsize);

	setprogname("master");
	if (verbose > 1)
		loginfox("%d", getpid());

	if (verbose > 1)
		printdescriptors();

	/*
	 *   3. send startup info to processes
	 */

	sendconfig_enclave(mastwithencl, enclwithprox);
	sendconfig_proxy(mastwithprox, proxwithencl);

	for (n = 0; n < ifnvsize; n++)
		sendconfig_ifn(n);

	/*
	 *   4. reexec and idle
	 */

	/*
	 * Pump config over a stream to our future-self
	 *
	 * wire format:
	 * enclave descriptor
	 * proxy descriptor
	 * number of ifn descriptors
	 * each ifn descriptor
	 * ...
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, mastmast) == -1)
		logexit(1, "socketpair mastermaster");
	if (writen(mastmast[0], &mastwithencl, sizeof(int)) != 0)
		logexit(1, "could not write enclave descriptor to new master");
	if (writen(mastmast[0], &mastwithprox, sizeof(int)) != 0)
		logexit(1, "could not write proxy descriptor to new master");
	if (writen(mastmast[0], &ifnvsize, sizeof(ifnvsize)) != 0)
		logexit(1, "could not write ifnvsize to new master");
	for (n = 0; n < ifnvsize; n++) {
		if (writen(mastmast[0], &ifnv[n]->mastwithifn, sizeof(int)) != 0)
			logexit(1, "could not pass ifn descriptor to new "
			    "master");
	}
	close(mastmast[0]);

	eargs[0] = (char *)getprogname();
	eargs[1] = "-M";
	if (asprintf(&eargs[2], "%u", mastmast[1]) < 1)
		logexitx(1, "asprintf");
	/* don't bother to free before exec */
	eargs[3] = NULL;
	execvpe(oldprogname, eargs, eenv);
	logexit(1, "exec master");
}

void
master_printinfo(FILE *fp)
{
	struct ifn *ifn;
	struct peer *peer;
	size_t n, m;

	for (n = 0; n < ifnvsize; n++) {
		ifn = ifnv[n];
		fprintf(fp, "ifn %zu\n", n);
		fprintf(fp, "mastwithifn %d\n", ifn->mastwithifn);
		fprintf(fp, "ifnwithmast %d\n", ifn->ifnwithmast);
		fprintf(fp, "enclwithifn %d\n", ifn->enclwithifn);
		fprintf(fp, "ifnwithencl %d\n", ifn->ifnwithencl);
		fprintf(fp, "proxwithifn %d\n", ifn->proxwithifn);
		fprintf(fp, "ifnwithprox %d\n", ifn->ifnwithprox);
		fprintf(fp, "ifname %s\n", ifn->ifname);
		fprintf(fp, "pubkey\n");
		hexdump(fp, ifn->pubkey, sizeof(ifn->pubkey), sizeof(ifn->pubkey));
		fprintf(fp, "pubkeyhash\n");
		hexdump(fp, ifn->pubkeyhash, sizeof(ifn->pubkeyhash), sizeof(ifn->pubkeyhash));
		fprintf(fp, "mac1key\n");
		hexdump(fp, ifn->mac1key, sizeof(ifn->mac1key), sizeof(ifn->mac1key));
		fprintf(fp, "cookiekey\n");
		hexdump(fp, ifn->cookiekey, sizeof(ifn->cookiekey), sizeof(ifn->cookiekey));

		for (m = 0; m < ifn->peerssize; m++) {
			peer = ifn->peers[m];
			fprintf(fp, "peer %zu\n", m);
			fprintf(fp, "pubkey\n");
			hexdump(fp, peer->pubkey, sizeof(peer->pubkey), sizeof(peer->pubkey));
			fprintf(fp, "mac1key\n");
			hexdump(fp, peer->mac1key, sizeof(peer->mac1key), sizeof(peer->mac1key));
		}

	}
}
