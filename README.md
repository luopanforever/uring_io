## 什么是`io_uring`:beers:

是内核版本`5.10`之后的产物，也就是你的内核版本要在`5.10`之后才能使用，**用户空间的环形队列**

看见其名字就知道，带队列，能够起到异步解耦的作用，它可以与`epoll`的性能相提并论，但是却与`epoll`的工作原理完全不同，下面就让我们来学习它

> **安装一个`liburing`库**
>
> ```bash
> git clone https://github.com/axboe/liburing.git
> ./configure
> make
> make install
> ```

## 工作原理:beers:

### 符号说明:symbols:

- `sq`：`submit`队列
- `cq`：`complete`队列
- `sqe`：`sq`上某一节点
- `cqe`：`cq`上某一节点

### 两个队列:question:

内核中有两个环形队列，其中一个是`submit queue`一个是`complete queue`，简称他们为`sq`与`cq`

- `sq`：用于用户发起操作请求的队列，例如用户发起一个`accept`(这个异步`accept`由`liburing`实现)之后会将这个`ACCEPT`请求封装为一个节点，放进`sq`的队列中，之后通过调用一个`submit`函数来将`sq`队列中的节点放入内核中去处理
- `cq`：用于内核处理完成后放入节点的位置，内核在异步处理完操作后会将节点放入`cq`队列中

**注意：**在上述描述中我用到**将节点放进**这个说法其实是不对的，这样说就好像有拷贝的动作，但是这整个过程中其实都没有拷贝的动作，`sq`与`cq`都维护的指针，指向的是对应节点，只是什么时候他们该指向哪个节点，例如内核处理完成后`cq`就会指向完成节点

### 共享内存

`io_uring`底层也有共享内存的部分，`sq`到`cq`中没有拷贝的动作，他们指向的都是一个内核与用户态共享的一块内存块

### 异步

通过队列，`io_uring`将`accep、recv、send`封装成了异步`io`

- 例如`accept`，假设`io_uring`给出的接口是`accept_prepare`，调用他后直接返回，`io_uring`将`accept`请求放入`sq`中，内核取出处理完毕后的放进`cq`中，通过`cq->res`获取原本系统调用的返回值，通过一些**附加信息**获取原始`sockfd`

## 使用`io_uring`:beers:

使用`io_uring`实现一个可供多个客户端连接的回声服务器

### 大概流程:flags:

- 初始化`sq`与`cq`队列
- 将`accept`操作注册进`sq`队列中
- `submit` `sq`队列中的操作到内核去处理
- 从`cq`中获取操作完成的操作们到用户态
- 循环遍历判断状态来获取相应的返回值以及进行相应处理

**注意：**当状态通知到时 操作就已经是完成了的，我们只需要直接读结果就行，而不是像`reactor`那样事件通知然后执行相应的操作

### 使用`io_uring`

首先写一个没有`accept`的`TCP`服务器 `hh`伪代码

```c++
int sockfd = socket(AF_INET, SOCK_STREAM, 0); // io
	
bind -> Ip:0.0.0.0 Port:9999
listen
```

### 获取原始`sockfd`

什么是原始`sockfd`:question:

- 例如`posixAPI`中`accpet`，第一个参数是一个`listenfd`，其返回值是`clientfd`，其`listenfd`就是原始`sockfd`
- 例如`posixAPI`中`recv`，第一个参数是`clientfd`，其返回值是实际读取到的字节，`其clientfd`就是原始`sockfd`
- 为什么要专门说获取原始`sockfd`，因为如果不做任何附加信息，`cq`中取出来的节点其上面只有原始函数的返回值，无法获取原始`sockfd`，因而有一个场景，如果想要连接多个客户端，在第一次`accpet`状态触发后需要重新注册`accept`操作进`sq`队列，此时进凭`cqe->res`是无法操作的，我们需要利用`cqe->user_data`

在`epoll`中用`epoll_event`来获取原始`fd`和注册的对应事件，而`io_uring`要获取原始`fd`，和设置操作状态 的话也是需要这样一个结构体，我们可以自己实现

```c++
enum {
	EVENT_ACCEPT = 0,
	EVENT_READ,
	EVENT_WRITE
};

typedef struct _conninfo {
	int connfd;
	int event;
} conninfo;
```

- 在`io_uring_sqe`结构体中有一个`64`位`ull`的成员`user_data`，所以我们设计一个`conninfo`的结构体来存储不同的操作状态和原始`sockfd`，**它后续是这样使用的**

  ```c++
  struct io_uring_sqe *sqe = get_sqe_from_ring();
  io_uring_prep_accept(sqe, sockfd, addr, addrlen, flag);
  conninfo info_accept = {
      .connfd = sockfd,
      .event = EVENT_ACCEPT,
  };
  // 将对应状态附给sqe
  memcpy(&sqe->user_data, &info_accept, sizeof(info_accept));
  ```

  - 在`cqe`(完成队列某节点)中我们就可以通过其`user_data`字段得到对应的描述符，根据其状态来决定下一步操作，比如如果状态为`EVENT_ACCEPT`则说明有客户端连接，首先将返回的`clientfd`获取，然后先将`listenfd`的**`accept`操作**注册进`sq`中（保证多个客户端可连接）然后再将`clientfd`的**`recv`操作**注册进`sq`中（使服务器能够接收数据）

    **注意：**最后的处理是将他们的**操作**注册进`sq`中，在代码上的形式就是调用了`io_uring`中异步的`accept`和`recv`，他们的返回值都是`void`，真正的返回值通过`cq`队列中节点的`res`字段获取

#### 初始化`sq`与`cq`队列:bee: `<io_uring_queue_init_params>`

```c++
#define ENTRIES_LENGTH		1024

struct io_uring_params params;
memset(&params, 0, sizeof(params));

struct io_uring ring;
io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params);
```

该函数执行后，`sq`与`cq`队列被初始化

- `io_uring`结构体中维护着`sq`与`cq`队列
- `ENTRIES_LENGTH`指定队列的长度
- `params`被初始化为`0`值，表示属性全部使用默认

#### 注册`accept`操作到`sq`队列:bee: `io_uring_prep_accept`

`io_uring`提供的异步`accept`只比`accept4`多了一个参数，也就是`sq`队列的地址

封装了一个函数

```c++
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
```

- 此函数主要是执行了异步的`accept`，并附加了状态信息以及描述符信息
  - 获取`sq`位置，
  - 调用异步`api`，
  - 加入附加信息

> 还有两个操作被封装成了函数，注册`read`操作与注册`write`操作到`sq`队列
>
> ```c++
> void set_send_event(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags) {
> 
> 	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
> 
> 	io_uring_prep_send(sqe, sockfd, buf, len, flags);
> 	conninfo info_send = {
> 		.connfd = sockfd,
> 		.event = EVENT_WRITE,
> 	};
> 	memcpy(&sqe->user_data, &info_send, sizeof(info_send));
> 
> }
> 
> void set_recv_event(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags) {
> 
> 	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
> 
> 	io_uring_prep_recv(sqe, sockfd, buf, len, flags);
> 	conninfo info_recv = {
> 		.connfd = sockfd,
> 		.event = EVENT_READ,
> 	};
> 	memcpy(&sqe->user_data, &info_recv, sizeof(info_recv));
> 
> }
> ```
>
> 与`accept`相似，只是内部调用`api`的不同与附加操作状态的不同

#### `mainloop`阶段:loop: `<while(1)>`

下面的操作都包含在一个`while(1)`里面

##### 提交`sq`上的操作到内核:rocket:`<io_uring_submit>`

```c++
io_uring_submit(&ring);
```

##### 主程序等待`cq`中有节点:arrow_left:`<io_uring_wait_cqe>`

```c++
struct io_uring_cqe *cqe_;
io_uring_wait_cqe(&ring, &cqe_);
```

##### 探测到`cq`中有节点后取出`cq`中指定个数的节点:arrow_left:`<io_uring_peek_batch_cqe>`

```c++
struct io_uring_cqe *cqes[10];
int cqecount = io_uring_peek_batch_cqe(&ring, cqes, 10);
```

- `cqecount <=` 第三个参数(这里是`10`)

##### **循环遍历每个节点，根据操作状态来决定下一步**:arrow_left:`<for(int i = 0; i < cqecount; i++)>`

取出对应操作的原始`sockfd`与操作状态
在`for`外面定义`struct io_uring_cqe *cqe;`供获取每个操作完成的节点

```c++
cqe = cqes[i];
// 取出里面的原始sockfd与操作状态
conninfo ci;
memcpy(&ci, &cqe->user_data, sizeof(ci));
```

- 状态：`ci.event`，原始`sockfd`：`ci.connfd`

###### 根据状态延伸出不同的操作

- `ci.event == EVENT_ACCEPT`

  ```c++
  int connfd = cqe->res;
  
  set_accept_event(&ring, ci.connfd, (struct sockaddr*)&clientaddr, &clilen, 0);
  
  set_recv_event(&ring, connfd, buffer, 1024, 0);
  ```

  重新注册`accept`是为了多个客户端可连接

  注册`read`是让服务器可以接受客户端发送数据

- `ci.event == EVENT_READ`

  ```c++
  if (cqe->res == 0) {
      close(ci.connfd);
  } else {
      printf("recv --> %s, %d\n", buffer, cqe->res);
      set_send_event(&ring, ci.connfd, buffer, cqe->res, 0);
  }
  ```

  - 通过`cqe->res`获取异步`read`操作的返回值，这里就能看出与`reactor`的区别，`reactor`是`read`事件触发了才开始执行`read`操作，这里当`read`状态通知时是`read`操作已经调用完成了，接着就直接注册`send`

- `ci.event == EVENT_WRITE`

  ```c++
  set_recv_event(&ring, ci.connfd, buffer, 1024, 0);
  ```

  - 能够执行到这里就说明`send`成功了，此时只需要再次设置`recv`客户端即可进行多次发送

##### 推进` I/O `事件完成队列的指针:eight_pointed_black_star:`<io_uring_cq_advance>`

在`for`循环完成之后调用

```c++
io_uring_cq_advance(&ring, cqecount);
```

- 这样下次调用`io_uring_peek_batch_cqe`获取`cqe`就不会出错了
