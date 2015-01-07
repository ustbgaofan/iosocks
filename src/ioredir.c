/*
 * ioredir.c - A transparent TCP proxy through remote osocks servers
 *
 * Copyright (C) 2014, Xiaoxiao <i@xiaoxiao.im>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <ev.h>
#include "conf.h"
#include "encrypt.h"
#include "log.h"
#include "md5.h"
#include "mem.h"

// 缓冲区大小
#define BUF_SIZE 8192

// 魔数
#define MAGIC 0x526f6e61

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

// 连接状态
typedef enum
{
	CLOSED = 0,
	ESTAB
} state_t;

// 连接控制块结构
typedef struct
{
	ev_io w_local_read;
	ev_io w_local_write;
	ev_io w_remote_read;
	ev_io w_remote_write;
	ssize_t rx_bytes;
	ssize_t tx_bytes;
	ssize_t rx_offset;
	ssize_t tx_offset;
	int sock_local;
	int sock_remote;
	state_t state;
	enc_evp_t enc_evp;
	uint8_t rx_buf[BUF_SIZE];
	uint8_t tx_buf[BUF_SIZE];
} conn_t;


static void help(void);
static void signal_cb(EV_P_ ev_signal *w, int revents);
static void accept_cb(EV_P_ ev_io *w, int revents);
static void connect_cb(EV_P_ ev_io *w, int revents);
static void local_read_cb(EV_P_ ev_io *w, int revents);
static void local_write_cb(EV_P_ ev_io *w, int revents);
static void remote_read_cb(EV_P_ ev_io *w, int revents);
static void remote_write_cb(EV_P_ ev_io *w, int revents);
static void cleanup(EV_P_ conn_t *conn);
static void rand_bytes(void *stream, size_t len);
static int setnonblock(int fd);
static int settimeout(int fd);
static int setreuseaddr(int fd);
static int setkeepalive(int fd);
static int getdestaddr(int fd, struct sockaddr *addr, socklen_t *addrlen);
static int geterror(int fd);

// 配置信息
conf_t conf;

// 服务器的信息
struct
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	char *key;
	size_t key_len;
} servers[MAX_SERVER];

int main(int argc, char **argv)
{
	const char *conf_file = NULL;

	// 处理命令行参数
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
		{
			help();
			return 0;
		}
		else if (strcmp(argv[i], "-c") == 0)
		{
			if (i + 2 > argc)
			{
				fprintf(stderr, "Invalid option: %s\n", argv[i]);
				return 1;
			}
			conf_file = argv[i + 1];
			i++;
		}
		else
		{
			fprintf(stderr, "Invalid option: %s\n", argv[i]);
			return 1;
		}
	}
	if (conf_file == NULL)
	{
		help();
		return 1;
	}
	if (read_conf(conf_file, &conf) != 0)
	{
		return 1;
	}
	if (conf.server_num == 0)
	{
		help();
		return 1;
	}
	for (int i = 0; i < conf.server_num; i++)
	{
		if (conf.server[i].address == NULL)
		{
			conf.server[i].address = "0.0.0.0";
		}
		if (conf.server[i].port == NULL)
		{
			conf.server[i].port = "1205";
		}
		if (conf.server[i].key == NULL)
		{
			help();
			return 1;
		}
	}
	if (conf.redir.address == NULL)
	{
		conf.redir.address = "127.0.0.1";
	}
	if (conf.redir.port == NULL)
	{
		conf.redir.port = "1081";
	}

	// 服务器信息
	struct addrinfo hints;
	struct addrinfo *res;
	for (int i = 0; i < conf.server_num; i++)
	{
		servers[i].key = conf.server[i].key;
		servers[i].key_len = strlen(servers[i].key);
		if (servers[i].key_len > 256)
		{
			servers[i].key[257] = '\0';
			servers[i].key_len = 256;
		}
		bzero(&hints, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(conf.server[i].address, conf.server[i].port, &hints, &res) != 0)
		{
			LOG("wrong server_host/server_port");
			return 2;
		}
		memcpy(&servers[i].addr, res->ai_addr, res->ai_addrlen);
		servers[i].addrlen = res->ai_addrlen;
		freeaddrinfo(res);
	}

	// 初始化内存池
	size_t block_size[1] = { sizeof(conn_t) };
	size_t block_count[1] = { 64 };
	if (mem_init(block_size, block_count, 1) != 0)
	{
		LOG("memory pool error");
		return 3;
	}

	// 初始化 ev_signal
	struct ev_loop *loop = EV_DEFAULT;
	ev_signal w_sigint;
	ev_signal w_sigterm;
	ev_signal_init(&w_sigint, signal_cb, SIGINT);
	ev_signal_init(&w_sigterm, signal_cb, SIGTERM);
	ev_signal_start(EV_A_ &w_sigint);
	ev_signal_start(EV_A_ &w_sigterm);

	// 初始化本地监听 socket
	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(conf.redir.address, conf.redir.port, &hints, &res) != 0)
	{
		LOG("wrong local_host/local_port");
		return 4;
	}
	int sock_listen = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
	if (sock_listen < 0)
	{
		ERR("socket");
		return 4;
	}
	setnonblock(sock_listen);
	setreuseaddr(sock_listen);
	if (bind(sock_listen, (struct sockaddr *)res->ai_addr, res->ai_addrlen) != 0)
	{
		ERR("bind");
		return 4;
	}
	freeaddrinfo(res);
	if (listen(sock_listen, 1024) != 0)
	{
		ERR("listen");
		return 4;
	}

	// 初始化 ev watcher
	ev_io w_listen;
	ev_io_init(&w_listen, accept_cb, sock_listen, EV_READ);
	ev_io_start(EV_A_ &w_listen);
	LOG("starting ioredir at %s:%s", conf.redir.address, conf.redir.port);

	// 执行事件循环
	ev_run(EV_A_ 0);

	// 退出
	close(sock_listen);
	LOG("Exit");

	return 0;
}

static void help(void)
{
	printf("usage: iodns\n"
		   "  -h, --help        show this help\n"
		   "  -c <config_file>  config file, see iosocks(8) for its syntax\n"
		   "");
}

static void signal_cb(EV_P_ ev_signal *w, int revents)
{
	assert((w->signum == SIGINT) || (w->signum == SIGTERM));
	ev_break(EV_A_ EVBREAK_ALL);
}

static void accept_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)mem_new(sizeof(conn_t));
	if (conn == NULL)
	{
		LOG("out of memory");
		return;
	}
	conn->sock_local = accept(w->fd, NULL, NULL);
	if (conn->sock_local < 0)
	{
		ERR("accept");
		mem_delete(conn);
		return;
	}
	setnonblock(conn->sock_local);
	settimeout(conn->sock_local);
	setkeepalive(conn->sock_local);
	conn->state = CLOSED;

	// 获取原始地址
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(struct sockaddr_storage);
	char host[257], port[15];
	if (getdestaddr(conn->sock_local, (struct sockaddr *)&addr, &addrlen) != 0)
	{
		ERR("getdestaddr");
		close(conn->sock_local);
		mem_delete(conn);
		return;
	}
	if (addr.ss_family == AF_INET)
	{
		inet_ntop(AF_INET, &(((struct sockaddr_in *)&addr)->sin_addr), host, INET_ADDRSTRLEN);
		sprintf(port, "%u", ntohs(((struct sockaddr_in *)&addr)->sin_port));
	}
	else
	{
		inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)&addr)->sin6_addr), host, INET6_ADDRSTRLEN);
		sprintf(port, "%u", ntohs(((struct sockaddr_in6 *)&addr)->sin6_port));
	}

	// 随机选择一个 server
	unsigned int index;
	rand_bytes(&index, sizeof(unsigned int));
	index %= (unsigned int)conf.server_num;
	LOG("connect %s:%s via %s:%s", host, port, conf.server[index].address, conf.server[index].port);

	// iosocks 请求
	// +-------+------+------+------+
	// | MAGIC | HOST | PORT |  IV  |
	// +-------+------+------+------+
	// |   4   | 257  |  15  | 236  |
	// +-------+------+------+------+
	uint8_t key[64];
	rand_bytes(conn->rx_buf, 236);
	memcpy(conn->rx_buf + 236, servers[index].key, servers[index].key_len);
	md5(conn->rx_buf, 236 + servers[index].key_len, key);
	md5(key, 16, key + 16);
	md5(key, 32, key + 32);
	md5(key, 48, key + 48);
	enc_init(&conn->enc_evp, enc_rc4, key, 64);
	memcpy(conn->tx_buf + 276, conn->rx_buf, 236);
	bzero(conn->tx_buf, 276);
	*((uint32_t *)(conn->tx_buf)) = htonl(MAGIC);
	strcpy((char *)conn->tx_buf + 4, host);
	strcpy((char *)conn->tx_buf + 261, port);
	io_encrypt(conn->tx_buf, 276, &conn->enc_evp);
	conn->tx_bytes = 512;
	// 建立远程连接
	conn->sock_remote = socket(servers[index].addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
	if (conn->sock_remote < 0)
	{
		ERR("socket");
		close(conn->sock_local);
		mem_delete(conn);
		return;
	}
	setnonblock(conn->sock_remote);
	settimeout(conn->sock_remote);
	setkeepalive(conn->sock_remote);
	ev_io_init(&conn->w_remote_write, connect_cb, conn->sock_remote, EV_WRITE);
	conn->w_remote_write.data = (void *)conn;
	ev_io_start(EV_A_ &conn->w_remote_write);
	connect(conn->sock_remote, (struct sockaddr *)&servers[index].addr, servers[index].addrlen);
}

static void connect_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);

	ev_io_stop(EV_A_ w);

	if (geterror(w->fd) == 0)
	{
		// 连接成功
		ev_io_init(&conn->w_remote_write, remote_write_cb, conn->sock_remote, EV_WRITE);
		conn->w_remote_write.data = (void *)conn;
		ev_io_start(EV_A_ &conn->w_remote_write);
	}
	else
	{
		// 连接失败
		LOG("connect to osocks failed");
		close(conn->sock_local);
		close(conn->sock_remote);
		mem_delete(conn);
	}
}

static void local_read_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);

	conn->tx_bytes = recv(conn->sock_local, conn->tx_buf, BUF_SIZE, 0);
	if (conn->tx_bytes <= 0)
	{
		if (conn->tx_bytes < 0)
		{
			LOG("client reset");
		}
		cleanup(EV_A_ conn);
		return;
	}
	io_encrypt(conn->tx_buf, conn->tx_bytes, &conn->enc_evp);
	ssize_t n = send(conn->sock_remote, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
	if (n < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			conn->tx_offset = 0;
		}
		else
		{
			ERR("send");
			cleanup(EV_A_ conn);
			return;
		}
	}
	else if (n < conn->tx_bytes)
	{
		conn->tx_offset = n;
		conn->tx_bytes -= n;
	}
	else
	{
		return;
	}
	ev_io_start(EV_A_ &conn->w_remote_write);
	ev_io_stop(EV_A_ w);
}


static void local_write_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)w->data;

	assert(conn != NULL);
	assert(conn->rx_bytes > 0);

	ssize_t n = send(conn->sock_local, conn->rx_buf + conn->rx_offset, conn->rx_bytes, MSG_NOSIGNAL);
	if (n < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			return;
		}
		else
		{
			ERR("send");
			cleanup(EV_A_ conn);
			return;
		}
	}
	else if (n < conn->rx_bytes)
	{
		conn->rx_offset += n;
		conn->rx_bytes -= n;
	}
	else
	{
		ev_io_start(EV_A_ &conn->w_remote_read);
		ev_io_stop(EV_A_ w);
	}
}

static void remote_read_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);

	conn->rx_bytes = recv(conn->sock_remote, conn->rx_buf, BUF_SIZE, 0);
	if (conn->rx_bytes <= 0)
	{
		if (conn->rx_bytes < 0)
		{
			LOG("server reset");
		}
		cleanup(EV_A_ conn);
		return;
	}
	io_decrypt(conn->rx_buf, conn->rx_bytes, &conn->enc_evp);
	ssize_t n = send(conn->sock_local, conn->rx_buf, conn->rx_bytes, MSG_NOSIGNAL);
	if (n < 0)
	{
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
		{
			conn->rx_offset = 0;
		}
		else
		{
			ERR("send");
			cleanup(EV_A_ conn);
			return;
		}
	}
	else if (n < conn->rx_bytes)
	{
		conn->rx_offset = n;
		conn->rx_bytes -= n;
	}
	else
	{
		return;
	}
	ev_io_start(EV_A_ &conn->w_local_write);
	ev_io_stop(EV_A_ w);
}

static void remote_write_cb(EV_P_ ev_io *w, int revents)
{
	conn_t *conn = (conn_t *)(w->data);

	assert(conn != NULL);

	if (conn->state == CLOSED)
	{
		ssize_t tx_bytes = send(conn->sock_remote, conn->tx_buf, conn->tx_bytes, MSG_NOSIGNAL);
		if (tx_bytes != conn->tx_bytes)
		{
			cleanup(EV_A_ conn);
			return;
		}
		else
		{
			conn->state = ESTAB;
			ev_io_init(&conn->w_local_read, local_read_cb, conn->sock_local, EV_READ);
			ev_io_init(&conn->w_local_write, local_write_cb, conn->sock_local, EV_WRITE);
			ev_io_init(&conn->w_remote_read, remote_read_cb, conn->sock_remote, EV_READ);
			conn->w_local_read.data = (void *)conn;
			conn->w_local_write.data = (void *)conn;
			conn->w_remote_read.data = (void *)conn;
			ev_io_start(EV_A_ &conn->w_local_read);
			ev_io_start(EV_A_ &conn->w_remote_read);
		}
	}
	else
	{
		assert(conn->tx_bytes > 0);
		ssize_t n = send(conn->sock_remote, conn->tx_buf + conn->tx_offset, conn->tx_bytes, MSG_NOSIGNAL);
		if (n < 0)
		{
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			{
				return;
			}
			else
			{
				ERR("send");
				cleanup(EV_A_ conn);
				return;
			}
		}
		else if (n < conn->tx_bytes)
		{
			conn->tx_offset += n;
			conn->tx_bytes -= n;
		}
		else
		{
			ev_io_start(EV_A_ &conn->w_local_read);
			ev_io_stop(EV_A_ w);
		}
	}
}

static void cleanup(EV_P_ conn_t *conn)
{
	ev_io_stop(EV_A_ &conn->w_local_read);
	ev_io_stop(EV_A_ &conn->w_local_write);
	ev_io_stop(EV_A_ &conn->w_remote_read);
	ev_io_stop(EV_A_ &conn->w_remote_write);
	close(conn->sock_local);
	close(conn->sock_remote);
	mem_delete(conn);
}

static int setnonblock(int fd)
{
	int flags;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
	{
		return -1;
	}
	if (-1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK))
	{
		return -1;
	}
	return 0;
}

static int settimeout(int fd)
{
	struct timeval timeout = { .tv_sec = 10, .tv_usec = 0};
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(struct timeval)) != 0)
	{
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(struct timeval)) != 0)
	{
		return -1;
	}
	return 0;
}

static int setreuseaddr(int fd)
{
	int reuseaddr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int)) != 0)
	{
		return -1;
	}
	return 0;
}

static int setkeepalive(int fd)
{
	int keepalive = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(int)) != 0)
	{
		return -1;
	}
	return 0;
}

static int geterror(int fd)
{
	int error = 0;
	socklen_t len = sizeof(int);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0)
	{
		return -1;
	}
	return error;
}

static int getdestaddr(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
	if (getsockopt(fd, SOL_IPV6, IP6T_SO_ORIGINAL_DST, addr, addrlen) == 0)
	{
		return 0;
	}
	if (getsockopt(fd, SOL_IP, SO_ORIGINAL_DST, addr, addrlen) == 0)
	{
		return 0;
	}
    return -1;
}

static void rand_bytes(void *stream, size_t len)
{
	static int urand = -1;
	if (urand == -1)
	{
		urand = open("/dev/urandom", O_RDONLY, 0);
	}
	read(urand, stream, len);
}