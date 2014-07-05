#include <getopt.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "common.h"
#include "crypto.h"
#include "log.h"

void usage_server(const char *name)
{
	printf("Usage: %s [options]\n", name);
	printf("Options:\n");
	printf("\t-l,--local local\n");
	printf("\t-b,--local-port local port\n");
	printf("\t-k,--password your password\n");
	printf("\t-m,--method encryption algorithm\n");
	printf("\t-d,--debug print debug information\n");
	printf("\t-v,--verbose print verbose information\n");
	printf("\t-h,--help print this help information\n");
}

/* read text from remote, encrypt and send to local */
int server_do_remote_read(int sockfd, struct link *ln)
{
	int ret;

	if (ln->state & SERVER_SEND_PENDING)
		return 0;

	ret = do_read(sockfd, ln, "text", 0);
	if (ret == -2) {
		goto out;
	} else if (ret == -1) {
		return 0;
	}

	if (ln->state & SS_UDP) {
			goto out;
	}

	if (encrypt(sockfd, ln) == -1)
		goto out;

	ret = do_send(ln->local_sockfd, ln, "cipher", 0);
	if (ret == -2) {
		goto out;
	} else if (ret == -1) {
		ln->state |= LOCAL_SEND_PENDING;
	}

	return 0;
out:
	return -1;
}

/* read cipher from local, decrypt and send to server */
int server_do_local_read(int sockfd, struct link *ln)
{
	int ret;

	if (ln->state & LOCAL_SEND_PENDING) {
		return 0;
	}

	/* if iv isn't received, wait to receive bigger than iv_len
	 * bytes before go to next step */
	if (ln->state & LOCAL_READ_PENDING) {
		ret = do_read(sockfd, ln, "cipher", ln->cipher_len);
		if (ret == -2) {
			goto out;
		} else if (ret == -1) {
			return 0;
		}

		if (ln->cipher_len <= iv_len) {
			return 0;
		} else {
			ln->state &= ~SERVER_READ_PENDING;
		}
	} else {
		ret = do_read(sockfd, ln, "cipher", 0);
		if (ret == -2) {
			goto out;
		} else if (ret == -1) {
			return 0;
		}

		if (!(ln->state & SS_IV_RECEIVED)) {
			if (ln->cipher_len <= iv_len) {
				ln->state |= LOCAL_READ_PENDING;
				return 0;
			}
		}
	}

	if (decrypt(sockfd, ln) == -1)
		goto out;

	if (ln->state & SS_UDP) {
		if (check_ss_header(sockfd, ln) == -1)
			goto out;
	} else if (!(ln->state & SS_TCP_HEADER_RECEIVED)) {
		if (check_ss_header(sockfd, ln) == -1)
			goto out;

		ln->state |= SS_TCP_HEADER_RECEIVED;

		if (ln->text_len == 0)
			return 0;
	}

	ret = do_send(ln->server_sockfd, ln, "text", 0);
	if (ret == -2) {
		goto out;
	} else if (ret == -1) {
		ln->state |= SERVER_SEND_PENDING;
	}

	return 0;
out:
	return -1;
}

int server_do_pollin(int sockfd, struct link *ln)
{
	if (sockfd == ln->local_sockfd) {
		if (ln->state & SERVER_PENDING) {
			sock_info(sockfd, "%s: server pending",
				  __func__);
			goto out;
		} else if (server_do_local_read(sockfd, ln) == -1) {
			goto clean;
		} else {
			goto out;
		}
	} else if (sockfd == ln->server_sockfd) {
		if (ln->state & LOCAL_PENDING) {
			sock_info(sockfd, "%s: local pending",
				  __func__);
			goto out;
		} else if (server_do_remote_read(sockfd, ln) == -1) {
			goto clean;
		} else {
			goto out;
		}
	}

out:
	return 0;
clean:
	sock_info(sockfd, "%s: close", __func__);
	destroy_link(ln);
	return -1;
}

int server_do_pollout(int sockfd, struct link *ln)
{
	int optval, ret;
	int optlen = sizeof(optval);

	/* write to local */
	if (sockfd == ln->local_sockfd) {
		if (ln->state & LOCAL_SEND_PENDING) {
			ret = do_send(sockfd, ln, "cipher", 0);
			if (ret == -2) {
				goto clean;
			} else if (ret == -1) {
				goto out;
			} else {
				ln->state &= ~LOCAL_SEND_PENDING;
				goto out;
			}
		} else {
			poll_rm(sockfd, POLLOUT);
		}
	} else {
		/* pending connect finished */
		if (!(ln->state & SERVER)) {
			if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR,
				       &optval, (void *)&optlen) == -1) {
				sock_warn(sockfd, "%s: getsockopt() %s",
					  __func__, strerror(errno));
				return -1;
			}

			if (optval == 0) {
				sock_info(sockfd,
					  "%s: pending connect() finished",
					  __func__);
				ln->time = time(NULL);
				ln->state |= SERVER;
			} else {
				sock_warn(sockfd,
					  "%s: pending connect() failed",
					  __func__);
				goto clean;
			}
		}

		if (ln->state & SERVER_SEND_PENDING) {
			ret = do_send(sockfd, ln, "text", 0);
			if (ret == -2) {
				goto clean;
			} else if (ret == -1) {
				goto out;
			} else {
				ln->state &= ~SERVER_SEND_PENDING;
				goto out;
			}
		} else {
			poll_rm(sockfd, POLLOUT);
		}
	}

out:
	return 0;
clean:
	sock_info(sockfd, "%s: close:", __func__);
	destroy_link(ln);
	return -1;
}

int main(int argc, char **argv)
{
	short revents;
	int i, listenfd, opt, sockfd;
	int ret = 0;
	char *local = NULL;
	char *l_port = NULL;
	struct link *ln;
	struct addrinfo *s_info = NULL;
	struct addrinfo *l_info = NULL;

	struct option long_options[] = {
		{"local", required_argument, 0, 'c'},
		{"local-port", required_argument, 0, 'b'},
		{"password", required_argument, 0, 'k'},
		{"method", required_argument, 0, 'm'},
		{"verbose", no_argument, 0, 'v'},
		{"debug", no_argument, 0, 'd'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "l:b:k:m:vdh",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'l':
			local = optarg;
			break;
		case 'b':
			l_port = optarg;
			break;
		case 'k':
			strncpy(password, optarg, MAX_KEY_LEN);
			password[MAX_KEY_LEN - 1] = '\0';
			break;
		case 'm':
			strncpy(method, optarg, MAX_METHOD_NAME_LEN);
			method[MAX_METHOD_NAME_LEN - 1] = '\0';
			break;
		case 'v':
			verbose = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'h':
			usage_server(argv[0]);
			exit(EXIT_SUCCESS);
		case '?':
			usage_server(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (local && l_port) {
		ret = getaddrinfo(local, l_port, NULL, &l_info);
		if (ret != 0) {
			printf("getaddrinfo error: %s\n", gai_strerror(ret));
			goto out;
		}
		pr_ai_info(l_info, "server listening address:");
	} else {
		printf("Either local addr or local port is not specified\n");
		usage_server(argv[0]);
		ret = -1;
		goto out;
	}

	if (crypto_init(password, method) == -1) {
		ret = -1;
		goto out;
	}

	poll_init();
	listenfd = do_listen(l_info, "tcp");
	clients[0].fd = listenfd;
	clients[0].events = POLLIN;
	listenfd = do_listen(l_info, "udp");
	clients[1].fd = listenfd;
	clients[1].events = POLLIN;

	while (1) {
		pr_info("start polling\n");
		ret = poll(clients, nfds, TCP_READ_TIMEOUT * 1000);
		if (ret == -1) {
			err_exit("poll error");
		} else if (ret == 0) {
			reaper();
			continue;
		}

		if (clients[0].revents & POLLIN) {
			sockfd = accept(clients[0].fd, NULL, NULL);
			if (sockfd == -1) {
				pr_warn("accept error\n");
			} else if (poll_set(sockfd, POLLIN) == -1) {
				close(sockfd);
			} else {
				ln = create_link(sockfd, "server");
				if (ln == NULL) {
					poll_del(sockfd);
					close(sockfd);
				}
			}
		}

		if (clients[1].revents & POLLIN) {
			pr_warn("udp socks5 not supported(for now)\n");
			/* ln = create_link(sockfd, "server"); */
			/* if (ln != NULL) { */
			/* 	check_ss_header(sockfd, ln); */
			/* } */
		}

		for (i = 2; i < nfds; i++) {
			sockfd = clients[i].fd;
			if (sockfd == -1)
				continue;

			revents = clients[i].revents;
			if (revents == 0)
				continue;

			ln = get_link(sockfd);
			if (ln == NULL) {
				sock_warn(sockfd, "close: can't get link");
				close(sockfd);
				continue;
			}

			if (revents & POLLIN) {
				server_do_pollin(sockfd, ln);
			}

			if (revents & POLLOUT) {
				server_do_pollout(sockfd, ln);
			}

			/* suppress the noise */
			/* if (revents & POLLPRI) { */
			/* 	sock_warn(sockfd, "POLLERR"); */
			/* } else if (revents & POLLERR) { */
			/* 	sock_warn(sockfd, "POLLERR"); */
			/* } else if (revents & POLLHUP) { */
			/* 	sock_warn(sockfd, "POLLHUP"); */
			/* } else if (revents & POLLNVAL) { */
			/* 	sock_warn(sockfd, "POLLNVAL"); */
			/* } */
		}

		reaper();
	}

out:
	crypto_exit();
	if (s_info)
		freeaddrinfo(s_info);
	if (l_info)
		freeaddrinfo(l_info);

	if (ret == -1)
		exit(EXIT_FAILURE);
	else
		exit(EXIT_SUCCESS);
}
