#include <pbs_config.h> /* the master config generated by configure */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "libpbs.h"
#include "dis.h"
#include "server_limits.h"
#include "net_connect.h"
#include "credential.h"
#include "pbs_version.h"
#include "pbs_ecl.h"

#define PBS_IFF_MAX_CONN_RETRIES 6

/**
 * @file	iff2.c
 * @brief
 * 	pbs_iff - authenticates the user to the PBS server.
 *
 * @par	Usage: call via pbs_connect() with
 *		pbs_iff [-t] hostname port [parent_connection_port]
 *		pbs_iff --version
 *
 *		The parent_connection_port is required unless -t (for test) is given.
 */
/*
 * Copyright (C) 1994-2021 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

int
main(int argc, char *argv[], char *envp[])
{
	int err = 0;
	pbs_net_t hostaddr = 0;
	int i;
	unsigned int parentport;
	int parentsock = -1;
	int parentsock_port = -1;
	short legacy = -1;
	uid_t myrealuid;
	struct passwd *pwent;
	int servport = -1;
	int sock;
	struct sockaddr_in sockname;
	pbs_socklen_t socknamelen;
	int testmode = 0;
	extern int optind;
	char *cln_hostaddr = NULL;

	/*the real deal or output pbs_version and exit?*/
	PRINT_VERSION_AND_EXIT(argc, argv);

	cln_hostaddr = getenv(PBS_IFF_CLIENT_ADDR);

	/* Need to unset LOCALDOMAIN if set, want local host name */

	for (i = 0; envp[i]; ++i) {
		if (!strncmp(envp[i], "LOCALDOMAIN=", 12)) {
			envp[i] = "";
			break;
		}
	}

	while ((i = getopt(argc, argv, "ti:")) != EOF) {
		switch (i) {
			case 't':
				testmode = 1;
				break;
			case 'i':
				cln_hostaddr = optarg;
				break;
			default:
				err = 1;
		}
	}
	if ((cln_hostaddr != NULL) && (testmode == 1)) {
		err = 1;
	}

	/* Keep the backward compatibility of pbs_iff.
	 * If the invoker component is older version,
	 * It will pass one lesser argument to pbs_iff.
	 * in case of test mode, the (argc - optind) should always be 2.
	 * Setting legacy true for testmode and correct num ot args,
	 * because getsockname() should be called on LOCAL socket
	 * to get the port number.
	 */
	if ((testmode && (argc - optind) == 2) ||
	    (!testmode && (argc - optind) == 3)) {
		legacy = 1;
	} else if ((!testmode && (argc - optind) == 4)) {
		legacy = 0;
	}

	if ((err == 1) || (legacy == -1)) {
		fprintf(stderr,
			"Usage: %s [-t] host port [parent_sock][parent_port]\n",
			argv[0]);
		fprintf(stderr, "       %s --version\n", argv[0]);
		return (1);
	}

	if (!testmode && isatty(fileno(stdout))) {
		fprintf(stderr, "pbs_iff: output is a tty & not test mode\n");
		return (1);
	}

	if (initsocketlib())
		return 1;

	/* first, make sure we have a valid server (host), and ports */

	if ((hostaddr = get_hostaddr(argv[optind])) == (pbs_net_t) 0) {
		fprintf(stderr, "pbs_iff: unknown host %s\n", argv[optind]);
		return (1);
	}
	if ((servport = atoi(argv[++optind])) <= 0)
		return (1);

	/* set single threaded mode */
	pbs_client_thread_set_single_threaded_mode();
	/* disable attribute verification */
	set_no_attribute_verification();

	/* initialize the thread context */
	if (pbs_client_thread_init_thread_context() != 0) {
		fprintf(stderr, "pbs_iff: thread initialization failed\n");
		return (1);
	}

	for (i = 0; i < PBS_IFF_MAX_CONN_RETRIES; i++) {
		sock = client_to_svr_extend(hostaddr, (unsigned int) servport, 1, cln_hostaddr);
		if (sock != PBS_NET_RC_RETRY)
			break;
		sleep(i * i + 1); /* exponential sleep increase */
	}
	if (sock < 0) {
		fprintf(stderr, "pbs_iff: cannot connect to host\n");
		if (i == PBS_IFF_MAX_CONN_RETRIES)
			fprintf(stderr, "pbs_iff: all reserved ports in use\n");
		return (4);
	}

	DIS_tcp_funcs();

	/* setup connection level thread context */
	if (pbs_client_thread_init_connect_context(sock) != 0) {
		fprintf(stderr, "pbs_iff: connect initialization failed\n");
		return (1);
	}

	if (testmode == 0) {
		/*legacy component will still take one argument less and will still have getsockname() call
		 * to get the parent port*/
		if ((parentsock = atoi(argv[++optind])) < 0)
			return (1);
		if (legacy == 0) {
			if ((parentsock_port = atoi(argv[++optind])) < 0)
				return (1);
		}
	} else {
		/* for test mode, use my own port rather than the parents */
		parentsock = sock;
	}

	/* next, get the real user name */

	myrealuid = getuid();
	pwent = getpwuid(myrealuid);
	if (pwent == NULL)
		return (3);

	/* now get the parent's client-side port */

	socknamelen = sizeof(sockname);

	/* getsockname()should be called in case of legacy
	 * or testmode.
	 */
	if (legacy == 1 || testmode) {
		if (getsockname(parentsock, (struct sockaddr *) &sockname, &socknamelen) < 0)
			return (3);
		parentport = ntohs(sockname.sin_port);
	} else
		parentport = ntohs(parentsock_port);

	pbs_errno = 0;
	err = tcp_send_auth_req(sock, parentport, pwent->pw_name, AUTH_RESVPORT_NAME, getenv(PBS_CONF_ENCRYPT_METHOD));
	if (err != 0 && pbs_errno != PBSE_BADCRED)
		return 2;

	err = pbs_errno;
	while (write(fileno(stdout), &err, sizeof(int)) == -1) {
		if (errno != EINTR)
			break;
	}
	if (pbs_errno != 0) {
		char *msg = get_conn_errtxt(sock);
		int len = 0;
		if (msg != NULL)
			len = strlen(msg);
		while (write(fileno(stdout), (char *) &len, sizeof(int)) == -1) {
			if (errno != EINTR)
				break;
		}
		if (len > 0) {
			while (write(fileno(stdout), msg, strlen(msg)) == -1) {
				if (errno != EINTR)
					break;
			}
		}
		return (1);
	}

	(void) close(sock);
	(void) fclose(stdout);
	return (0);
}
