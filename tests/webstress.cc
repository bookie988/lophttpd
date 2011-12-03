/* webstress webserver download bandwidht tesing tool
 * (C) 2011 Sebastian Krahmer
 */
#include <iostream>
#include <map>
#include <string>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>


using namespace std;


enum {
	HTTP_STATE_CONNECTING = 0,
	HTTP_STATE_CONNECTED,
	HTTP_STATE_TRANSFERING
};


class webstress {

	string host, port, path, err;
	bool sequential;
	int max_cl, peers, hdr_fail, write_fail, read_fail;
	time_t now;

	pollfd *pfds;

	struct client {
		size_t obtained, content_length;
		int state;
		time_t time, start_time;
	};

	map<int, client *> clients;

	static const int TIMEOUT = 5;

public:
	webstress(const string &h, const string &p, const string &f, bool seq = 0)
		: host(h), port(p), path(f), err(""), sequential(seq), max_cl(1024),
		  peers(0), hdr_fail(0), write_fail(0), read_fail(0), pfds(NULL)
	{
	}

	~webstress()
	{
	}

	void max_clients(int n)
	{
		max_cl = n;
	}


	int loop();

	int cleanup(int);

	void print_stat(int);

	const char *why()
	{
		return err.c_str();
	}

};


int writen(int fd, const void *buf, size_t len)
{
	int o = 0, n;
	char *ptr = (char*)buf;

	while (len > 0) {
		if ((n = write(fd, ptr+o, len)) <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return o;
			return n;
		}
		len -= n;
		o += n;
	}
	return o;
}


int webstress::cleanup(int fd)
{
	if (fd < 0)
		return -1;
	shutdown(fd, SHUT_RDWR);
	close(fd);
	pfds[fd].fd = -1;
	pfds[fd].events = pfds[fd].revents = 0;

	delete clients[fd];
	clients[fd] = NULL;
	--peers;
	return 0;
}


void webstress::print_stat(int fd)
{
	if (now - clients[fd]->start_time == 0)
		--clients[fd]->start_time;

	printf("[%05u][rf=%08u][hdrf=%08u][wf=%09u][%s][%f MB/s]\n", peers, 
	       read_fail, hdr_fail, write_fail,
	       path.c_str(),
	       (double)clients[fd]->content_length/(now - clients[fd]->start_time)/(1024*1024));
}


int webstress::loop()
{
	struct rlimit rl;

	if (geteuid() == 0) {
		rl.rlim_cur = (1<<16);
		rl.rlim_max = rl.rlim_cur;
		if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
			err = "webstress::loop::setrlimit:";
			err += strerror(errno);
			return -1;
		}
	} else {
		if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
			err = "webstress::loop::getrlimit:";
			err += strerror(errno);
			return -1;
		}
	}


	struct addrinfo *ai = NULL;
	int r = 0;
	if ((r = getaddrinfo(host.c_str(), port.c_str(), NULL, &ai)) < 0) {
		err = "webstress::loop::getaddrinfo:";
		err += gai_strerror(r);
		return -1;
	}

	pfds = new (nothrow) pollfd[rl.rlim_cur];
	if (!pfds) {
		err = "webstress::loop::OOM";
		return -1;
	}

	for (unsigned int i = 0; i < rl.rlim_cur; ++i)
		pfds[i].fd = -1;

	char GET[1024], buf[4096];

	snprintf(GET, sizeof(GET), "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n", path.c_str(),
	         host.c_str());
	size_t GET_len = strlen(GET);

	int sock = 0, fl = 0;
	for (;;) {
		now = time(NULL);
		while (peers + 10 < max_cl) {
			if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
				err = "webstress::loop::socket:";
				err += strerror(errno);
				return -1;
			}
			fl = fcntl(sock, F_GETFL);
			fcntl(sock, F_SETFL, fl|O_NONBLOCK);
			if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0 &&
			    errno != EINPROGRESS) {
				err = "webstress::loop::";
				err += strerror(errno);
				return -1;
			}
			client *c = new client;
			memset(c, 0, sizeof(client));
			c->state = HTTP_STATE_CONNECTING;
			c->time = now;
			clients[sock] = c;

			pfds[sock].fd = sock;
			pfds[sock].events = POLLIN|POLLOUT;
			pfds[sock].revents = 0;
			++peers;
		}

		if (poll(pfds, rl.rlim_cur, 100) < 0)
			continue;

		// starts at most at FD 3
		for (unsigned int i = 3; i < rl.rlim_cur; ++i) {
			if (!clients[i])
				continue;
			if (pfds[i].revents == 0 && now - clients[i]->time > TIMEOUT) {
				cleanup(i);
				continue;
			}
			if (pfds[i].revents == 0)
				continue;

			if (clients[i]->state == HTTP_STATE_CONNECTING) {
				int e = 0;
				socklen_t elen = sizeof(e);
				getsockopt(i, SOL_SOCKET, SO_ERROR, &e, &elen);
				if (e > 0) {
					cleanup(i);
					continue;
				}
				if (writen(i, GET, GET_len) <= 0) {
					++write_fail;
					cleanup(i);
					continue;
				}

				clients[i]->state = HTTP_STATE_CONNECTED;
				pfds[i].revents = 0;
				pfds[i].events = POLLIN;
				clients[i]->start_time = clients[i]->time = now;
			// just read header and extracet Content-Length if found
			} else if (clients[i]->state == HTTP_STATE_CONNECTED) {
				memset(buf, 0, sizeof(buf));
				if ((r = recv(i, buf, sizeof(buf) - 1, MSG_PEEK)) <= 0) {
					cleanup(i);
					continue;
				}
				char *ptr = NULL;
				if ((ptr = strstr(buf, "\r\n\r\n")) == NULL) {
					if (now - clients[i]->time > TIMEOUT) {
						++hdr_fail;
						cleanup(i);
					}
					continue;
				}
				if (read(i, buf, ptr - buf + 4) <= 0) {
					++hdr_fail;
					cleanup(i);
					continue;
				}
				clients[i]->state = HTTP_STATE_TRANSFERING;
				clients[i]->time = now;

				char *end_ptr = buf + (ptr - buf + 4);
				if ((ptr = strcasestr(buf, "Content-Length:")) != NULL) {
					ptr += 15;
					for (;ptr < end_ptr; ++ptr) {
						if (*ptr != ' ')
							break;
					}
					if (ptr >= end_ptr) {
						cleanup(i);
						continue;
					}
					clients[i]->content_length = strtoul(ptr, NULL, 10);
				} else
					clients[i]->content_length = (size_t)-1;
				pfds[i].revents = 0;
				pfds[i].events = POLLIN;

			// read content
			} else if (clients[i]->state == HTTP_STATE_TRANSFERING) {
				if ((r = read(i, buf, sizeof(buf))) < 0) {
					++read_fail;
					cleanup(i);
					continue;
				}
				clients[i]->obtained += r;
				if (clients[i]->obtained == clients[i]->content_length || r == 0) {
					print_stat(i);
					cleanup(i);
					continue;
				}
				pfds[i].revents = 0;
				pfds[i].events = POLLIN;
				clients[i]->time = now;
			} else {
				cleanup(i);
			}
		}

	}
	return 0;
}


int main(int argc, char **argv)
{
	webstress ws("127.0.0.1", "80", "/zero", 0);

	if (ws.loop() < 0)
		cerr<<ws.why()<<endl;

	return 1;
}
