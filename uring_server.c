#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <liburing.h>

#define ENTRIES_LENGTH		1024

enum {
	EVENT_ACCEPT = 0,
	EVENT_READ,
	EVENT_WRITE
};

typedef struct _conninfo {
	int connfd;
	int event;
} conninfo;

// sizeof(conninfo)  = 8

// 0, 1, 2
// 3, 4, 5

void set_send_event(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	io_uring_prep_send(sqe, sockfd, buf, len, flags);
	conninfo info_send = {
		.connfd = sockfd,
		.event = EVENT_WRITE,
	};
	memcpy(&sqe->user_data, &info_send, sizeof(info_send));

}


void set_recv_event(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	io_uring_prep_recv(sqe, sockfd, buf, len, flags);
	conninfo info_recv = {
		.connfd = sockfd,
		.event = EVENT_READ,
	};
	memcpy(&sqe->user_data, &info_recv, sizeof(info_recv));
}

void set_accept_event(struct io_uring *ring, int sockfd, struct sockaddr *addr,
                   socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	
    io_uring_prep_accept(sqe, sockfd, addr, addrlen, flags);
    conninfo info_accept = {
        .connfd = sockfd,
        .event = EVENT_ACCEPT,
    };
    memcpy(&sqe->user_data, &info_accept, sizeof(info_accept));

}


int main() {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0); // io
	
	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(struct sockaddr_in)); // 192.168.2.123
	servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
    servaddr.sin_port = htons(9999);

    if (-1 == bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr))) {
		printf("bind failed: %s", strerror(errno));
		return -1;
    }

    listen(sockfd, 10); 
//liburing

	struct io_uring_params params;
	memset(&params, 0, sizeof(params));

	struct io_uring ring;
	io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params);



	struct sockaddr_in clientaddr;
	socklen_t clilen = sizeof(struct sockaddr);
	
	set_accept_event(&ring, sockfd, (struct sockaddr*)&clientaddr, &clilen, 0);

	char buffer[1024] = {0};

	while (1) {

		io_uring_submit(&ring);

		struct io_uring_cqe *cqe_;  // 可以用其他
		io_uring_wait_cqe(&ring, &cqe_);

		struct io_uring_cqe *cqes[10];
		int cqecount = io_uring_peek_batch_cqe(&ring, cqes, 10);

		struct io_uring_cqe *cqe;
		//printf("cqecount --> %d\n", cqecount);
		int i = 0;
		for (i = 0; i < cqecount; i++) {

			cqe = cqes[i];

			conninfo ci;
			memcpy(&ci, &cqe->user_data, sizeof(ci));

			if (ci.event == EVENT_ACCEPT) {  // recv/send

				
				//if (cqe->res < 0) continue;

				int connfd = cqe->res;
				printf("accept --> %d\n", connfd);
				set_accept_event(&ring, ci.connfd, (struct sockaddr*)&clientaddr, &clilen, 0);
				
				set_recv_event(&ring, connfd, buffer, 1024, 0);
				
			} else if (ci.event == EVENT_READ) {

				//if (cqe->res < 0) continue;
				if (cqe->res == 0) {
				
					close(ci.connfd);
					
				} else {

					printf("recv --> %s, %d\n", buffer, cqe->res);

					//set_recv_event(&ring, ci.connfd, buffer, 1024, 0);
					set_send_event(&ring, ci.connfd, buffer, cqe->res, 0);
				}

			} else if (ci.event == EVENT_WRITE) { //
				// 到这里 写操作已经完成cqe->res代表实际写了多少个字节
				
				printf("write complete, write %d Bytes\n", cqe->res);
				set_recv_event(&ring, ci.connfd, buffer, 1024, 0);

			}
			

		}

		io_uring_cq_advance(&ring, cqecount);

	}


    getchar();

}




