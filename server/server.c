#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Definitions from csapp.h
#define	MAXLINE	 8192  /* Max text line length */
#define LISTENQ  1024  /* Second argument to listen() */
#define RIO_BUFSIZE 8192


// Definitions from client.c
// Dimensions for the drawn grid (should be GRIDSIZE * texture dimensions)
#define GRID_DRAW_WIDTH 640
#define GRID_DRAW_HEIGHT 640

#define WINDOW_WIDTH GRID_DRAW_WIDTH
#define WINDOW_HEIGHT (HEADER_HEIGHT + GRID_DRAW_HEIGHT)

// Header displays current score
#define HEADER_HEIGHT 50

// Number of cells vertically/horizontally in the grid
#define GRIDSIZE 10

typedef struct sockaddr SA;

typedef struct {
    int rio_fd;                /* Descriptor for this internal buf */
    int rio_cnt;               /* Unread bytes in internal buf */
    char *rio_bufptr;          /* Next unread byte in internal buf */
    char rio_buf[RIO_BUFSIZE]; /* Internal buffer */
} rio_t;

void unix_error(char *msg) /* Unix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(0);
}

void gai_error(int code, char *msg) /* Getaddrinfo-style error */
{
    fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
    exit(0);
}

void app_error(char *msg) /* Application error */
{
    fprintf(stderr, "%s\n", msg);
    exit(0);
}

void rio_readinitb(rio_t *rp, int fd) 
{
    rp->rio_fd = fd;  
    rp->rio_cnt = 0;  
    rp->rio_bufptr = rp->rio_buf;
}

ssize_t rio_writen(int fd, void *usrbuf, size_t n) 
{
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0) {
	if ((nwritten = write(fd, bufp, nleft)) <= 0) {
	    if (errno == EINTR)  /* Interrupted by sig handler return */
		nwritten = 0;    /* and call write() again */
	    else
		return -1;       /* errno set by write() */
	}
	nleft -= nwritten;
	bufp += nwritten;
    }
    return n;
}

void Rio_writen(int fd, void *usrbuf, size_t n) 
{
    if (rio_writen(fd, usrbuf, n) != n)
	unix_error("Rio_writen error");
}

void Rio_readinitb(rio_t *rp, int fd)
{
    rio_readinitb(rp, fd);
} 

static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {  /* Refill if buf is empty */
	rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, 
			   sizeof(rp->rio_buf));
	if (rp->rio_cnt < 0) {
	    if (errno != EINTR) /* Interrupted by sig handler return */
		return -1;
	}
	else if (rp->rio_cnt == 0)  /* EOF */
	    return 0;
	else 
	    rp->rio_bufptr = rp->rio_buf; /* Reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;          
    if (rp->rio_cnt < n)   
	cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

void Getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, 
                 size_t hostlen, char *serv, size_t servlen, int flags)
{
    int rc;

    if ((rc = getnameinfo(sa, salen, host, hostlen, serv, 
                          servlen, flags)) != 0) 
        gai_error(rc, "Getnameinfo error");
}

void Close(int fd) 
{
    int rc;

    if ((rc = close(fd)) < 0)
	unix_error("Close error");
}

int Accept(int s, struct sockaddr *addr, socklen_t *addrlen) 
{
    int rc;

    if ((rc = accept(s, addr, addrlen)) < 0)
	unix_error("Accept error");
    return rc;
}

int open_listenfd(char *port) 
{
    struct addrinfo hints, *listp, *p;
    int listenfd, rc, optval=1;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;             /* Accept connections */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... on any IP address */
    hints.ai_flags |= AI_NUMERICSERV;            /* ... using port number */
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo failed (port %s): %s\n", port, gai_strerror(rc));
        return -2;
    }

    /* Walk the list for one that we can bind to */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) 
            continue;  /* Socket failed, try the next */

        /* Eliminates "Address already in use" error from bind */
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,    //line:netp:csapp:setsockopt
                   (const void *)&optval , sizeof(int));

        /* Bind the descriptor to the address */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* Success */
        if (close(listenfd) < 0) { /* Bind failed, try the next */
            fprintf(stderr, "open_listenfd close failed: %s\n", strerror(errno));
            return -1;
        }
    }


    /* Clean up */
    freeaddrinfo(listp);
    if (!p) /* No address worked */
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0) {
        close(listenfd);
	return -1;
    }
    return listenfd;
}

int Open_listenfd(char *port) 
{
    int rc;

    if ((rc = open_listenfd(port)) < 0)
	unix_error("Open_listenfd error");
    return rc;
}

ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) { 
        if ((rc = rio_read(rp, &c, 1)) == 1) {
	    *bufp++ = c;
	    if (c == '\n') {
                n++;
     		break;
            }
	} else if (rc == 0) {
	    if (n == 1)
		return 0; /* EOF, no data read */
	    else
		break;    /* EOF, some data was read */
	} else
	    return -1;	  /* Error */
    }
    *bufp = 0;
    return n-1;
}

ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
	unix_error("Rio_readlineb error");
    return rc;
} 

void Fputs(const char *ptr, FILE *stream) 
{
    if (fputs(ptr, stream) == EOF)
	unix_error("Fputs error");
}

char *Fgets(char *ptr, int n, FILE *stream) 
{
    char *rptr;

    if (((rptr = fgets(ptr, n, stream)) == NULL) && ferror(stream))
	app_error("Fgets error");

    return rptr;
}

void posix_error(int code, char *msg) /* Posix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(code));
    exit(0);
}

void Pthread_create(pthread_t *tidp, pthread_attr_t *attrp, 
		    void * (*routine)(void *), void *argp) 
{
    int rc;

    if ((rc = pthread_create(tidp, attrp, routine, argp)) != 0)
	posix_error(rc, "Pthread_create error");
}



void Sem_init(sem_t *sem, int pshared, unsigned int value) 
{
    if (sem_init(sem, pshared, value) < 0)
	unix_error("Sem_init error");
}

void P(sem_t *sem) 
{
    if (sem_wait(sem) < 0)
	unix_error("P error");
}

void V(sem_t *sem) 
{
    if (sem_post(sem) < 0)
	unix_error("V error");
}

void Pthread_detach(pthread_t tid) {
    int rc;

    if ((rc = pthread_detach(tid)) != 0)
	posix_error(rc, "Pthread_detach error");
}

void Free(void *ptr) 
{
    free(ptr);
}

void *Malloc(size_t size) 
{
    void *p;

    if ((p  = malloc(size)) == NULL)
	unix_error("Malloc error");
    return p;
}

// GAME CODE
typedef struct
{
    int x;
    int y;
} Position;

typedef enum
{
    TILE_GRASS,
    TILE_TOMATO
} TILETYPE;

TILETYPE grid[GRIDSIZE][GRIDSIZE];

Position playerPosition[4];
int score;
int level;
int numTomatoes;
int playerCount;
int num;
bool playerNumber[4];
int connections[4];
sem_t mutex;

void printGrid() {
	printf("GRID\n");
	for (int i = 0; i < GRIDSIZE; i++) {
		for (int j = 0; j < GRIDSIZE; j++) {
				if (i == playerPosition[0].x && j == playerPosition[0].y) {
					printf("P");
				}
				else if (i == playerPosition[1].x && j == playerPosition[1].y) {
					printf("A");
				}
				else if (i == playerPosition[2].x && j == playerPosition[2].y) {
					printf("B");
				}
				else if (i == playerPosition[3].x && j == playerPosition[3].y) {
					printf("C");
				}
				else if (grid[i][j] == TILE_TOMATO) {
					printf("T");
				}
				else if (grid[i][j] == TILE_GRASS) {
					printf("G");
				}
			}
		printf("\n");
		}
}

// get a random value in the range [0, 1]
double rand01()
{
    return (double) rand() / (double) RAND_MAX;
}

void initGrid()
{
    for (int i = 0; i < GRIDSIZE; i++) {
        for (int j = 0; j < GRIDSIZE; j++) {
            double r = rand01();
            if (r < 0.1) {
                grid[i][j] = TILE_TOMATO;
                numTomatoes++;
            }
            else
                grid[i][j] = TILE_GRASS;
        }
    }
    
    for (int i = 0; i < 4; i++) {
    	int a = playerPosition[i].x;
    	int b = playerPosition[i].y;
    	if (a < 0 || b < 0) {
    		continue;
    	}
    	else {
    		if (grid[a][b] == TILE_TOMATO) {
    			grid[a][b] = TILE_GRASS;
    			numTomatoes--;
    		}
    	}
    }

    // ensure grid isn't empty
    while (numTomatoes == 0)
        initGrid();
}

void moveTo(int x, int y, int player)
{
	P(&mutex);
    // Prevent falling off the grid
    if (x < 0 || x >= GRIDSIZE || y < 0 || y >= GRIDSIZE) {
    V(&mutex);
        return;
        }

    // Sanity check: player can only move to 4 adjacent squares
    if (!(abs(playerPosition[player].x - x) == 1 && abs(playerPosition[player].y - y) == 0) &&
        !(abs(playerPosition[player].x - x) == 0 && abs(playerPosition[player].y - y) == 1)) {
        fprintf(stderr, "Invalid move attempted from (%d, %d) to (%d, %d)\n", playerPosition[player].x, playerPosition[player].y, x, y);
        V(&mutex);
        return;
    }
    for (int i = 0; i < 4; i++) {
    	if (i == player) {
    		continue;
    	}
    	if (x == playerPosition[i].x && y == playerPosition[i].y) {
    		V(&mutex);
    		return;
    	}
    }

    playerPosition[player].x = x;
    playerPosition[player].y = y;
    
    if (grid[x][y] == TILE_TOMATO) {
        grid[x][y] = TILE_GRASS;
        score++;
        numTomatoes--;
        if (numTomatoes == 0) {
            level++;
            initGrid();
        }
    }
    V(&mutex);
}

void removePlayer(int player) {
	playerPosition[player].x = -1;
	playerPosition[player].y = -1;
	playerNumber[player] = false;
	P(&mutex);
	playerCount--;
	V(&mutex);
}

bool initializePlayer(int player) {
	playerNumber[player] = true;
	bool next = false;
	P(&mutex);
	for (int i = 0; i < GRIDSIZE; i++) {
		for (int j = 0; j < GRIDSIZE; j++) {
			if (grid[i][j] == TILE_GRASS) {
			next = false;
    for (int z = 0; z < 4; z++) {
    	if (z == player) {
    		continue;
    	}
    	if (i == playerPosition[z].x && j == playerPosition[z].y) {
    		next = true;
    		break;
    	}
    }
    if (next) {
    	continue;
    }
				playerPosition[player].x = i;
				playerPosition[player].y = j;
				V(&mutex);
				return true;
			}
		}
	}
	V(&mutex);
	return false;
}

bool processinput(char* buf, int player) {
	if (strcmp(buf, "quit\n") == 0) {
		return true;
	}
	if (strcmp(buf, "up\n") == 0) {
		moveTo(playerPosition[player].x, playerPosition[player].y - 1, player);
	}
	if (strcmp(buf, "down\n") == 0) {
		moveTo(playerPosition[player].x, playerPosition[player].y + 1, player);
	}
	if (strcmp(buf, "left\n") == 0) {
		moveTo(playerPosition[player].x - 1, playerPosition[player].y, player);
	}
	if (strcmp(buf, "right\n") == 0) {
		moveTo(playerPosition[player].x + 1, playerPosition[player].y, player);
	}
	return false;
}

void update(int connfd, char *buf, int player) {
	char s[10];
		memset(buf, 0, MAXLINE);
		for (int i = 0; i < GRIDSIZE; i++) {
			for (int j = 0; j < GRIDSIZE; j++) {
				if (i == playerPosition[0].x && j == playerPosition[0].y) {
					strcat(buf, "P");
				}
				else if (i == playerPosition[1].x && j == playerPosition[1].y) {
					strcat(buf, "A");
				}
				else if (i == playerPosition[2].x && j == playerPosition[2].y) {
					strcat(buf, "B");
				}
				else if (i == playerPosition[3].x && j == playerPosition[3].y) {
					strcat(buf, "C");
				}
				else if (grid[i][j] == TILE_TOMATO) {
					strcat(buf, "T");
				}
				else if (grid[i][j] == TILE_GRASS) {
					strcat(buf, "G");
				}
			}
		}
		// add score, level at back, playerposition at back
		memset(s, 0, 10);
		sprintf(s, "%d", score+10000);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", level+10000);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[0].x + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[0].y + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[1].x + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[1].y + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[2].x + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[2].y + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[3].x + 20);
		strcat(buf, s);
		memset(s, 0, 10);
		sprintf(s, "%d", playerPosition[3].y + 20);
		strcat(buf, s);
		
		strcat(buf, "\n");
		Rio_writen(connfd, buf, strlen(buf));
}

void *updateGame(void *vargp) {
	int connfd = *((int*)vargp);
	Pthread_detach(pthread_self());
	Free(vargp);
	int player = num;
	P(&mutex);
	num++;
	V(&mutex);
	int i = 0;
	int j = 0;
	size_t n;
	char buf[MAXLINE], s[10];
	rio_t rio;
	Rio_readinitb(&rio, connfd);
	// Give initial game state
	Rio_readlineb(&rio, buf, MAXLINE);
	P(&mutex);
		while (i < playerCount) {
			if (playerNumber[j] == true) {
				update(connections[j], buf, j);
				i++;
			}
			j++;
		}
		V(&mutex);
	while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
		if(processinput(buf, player)) {
			break;
		}
		i = 0;
		j = 0;
		P(&mutex);
		while (i < playerCount) {
			if (playerNumber[j] == true) {
				update(connections[j], buf, j);
				i++;
			}
			j++;
		}
		V(&mutex);
	}
		removePlayer(player);
		printf("Closing connection\n");
		i = 0;
		j = 0;
		P(&mutex);
		while (i < playerCount) {
			if (playerNumber[j] == true) {
				update(connections[j], buf, j);
				i++;
			}
			j++;
		}
		V(&mutex);
		Close(connfd);
	return NULL;
}

int main(int argc, char **argv) {
	// initialize the map
	Sem_init(&mutex,0,1);
	num = 0;
	srand(time(NULL));
	pthread_t tid;
	level = 1;
	playerCount = 0;
	removePlayer(0);
	removePlayer(1);
	removePlayer(2);
	removePlayer(3);
	playerCount = 0;
	initGrid();
	// accept connections
	int listenfd, *connfdp;
	char hostname[MAXLINE], port[MAXLINE];
	socklen_t clientlen;
	struct sockaddr_storage clientaddr; // enough room for any address
	
	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	listenfd = Open_listenfd(argv[1]);
	// playing the game
	while (1) {
		while (playerCount >= 4) {
			;
		}
		clientlen = sizeof(clientaddr);
		connfdp = Malloc(sizeof(int));
		*connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
		Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
		for (int i = 0; i < 4; i++) {
			if (playerNumber[i] == false) {
				num = i;
				break;
			}
		}
		connections[num] = *((int*)connfdp);
		initializePlayer(num);
		printf("Accepted connection from (%s, %s)\n", hostname, port);
		Pthread_create(&tid, NULL, updateGame, connfdp);
		P(&mutex);
		playerCount++;
		V(&mutex);
	}
}
