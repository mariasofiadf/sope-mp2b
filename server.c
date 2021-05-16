#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <semaphore.h>
#include "common.h"
#include "lib.h"

#define PERM 0666
#define CONFORTSIZE	1024

int timeout = 0;
int finish = 0;
int serverfifoclosed = 0;

typedef struct {
    Message * buffer;
    int read_index;
    int write_index;
    int length;
}circular_buff;

circular_buff * circ_buffer;
int buffer_size = 100;

pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
char *serverfifoname = NULL;
int serverfifo = -1;

sem_t sem;

enum oper{
    RECVD,
    TSKEX,
    TSKDN,
    TOOLATE,
    FAILD
};

int load_args(int argc, char** argv);
void print_usage();
void alrm(int);
void pips(int signo);
int setup_sigalrm();
int setup_sigpips();
void register_op(int i, int t, int res, enum oper oper);
void send_to_client(Message * message);
void * thread_producer(void* a);
void join_threads();
circular_buff * init_circ_buff();
int write_to_buff(circular_buff * circ_buff, Message * message);
int read_from_buff(circular_buff * circ_buff, Message * message);

sem_t reader_sem, writer_sem;

int write_count = 0;
int read_count = 0;

void * thread_producer(void* a){

    fprintf(stderr,"[server] producer thread starting: %ld\n", pthread_self());
    Message * request = malloc(sizeof(Message));
    request = (Message *) a;
    if(!finish){
        int res = task(request->tskload);
        request->tskres = res;
        register_op(request->rid, request->tskload, request->tskres, TSKEX);
    }
    else{
        request->tskres = -1;
    }

    write_to_buff(circ_buffer, request);

    free(request);

    fprintf(stderr,"[server] producer thread terminating: %ld\n", pthread_self());
    pthread_exit(a);
}

void * thread_consumer(void *a){

    fprintf(stderr, "[server] consumer thread starting\n");

	Message *request = malloc(sizeof(Message));

    char clientfifoname[CONFORTSIZE];

	int clientfifo = -1;

    while(1){
        clientfifo = -1;

        read_from_buff(circ_buffer, request);

        sprintf(clientfifoname, "/tmp/%d.%lu", request->pid, (unsigned long) request->tid);

        while ((clientfifo = open(clientfifoname, O_WRONLY)) < 0) {
            register_op(request->tid, request->tskload, request->tskres, FAILD);
		    perror("[server] open clientfifo");
            fprintf(stderr, "%s\n", clientfifoname);
			break;
	    }
        if(clientfifo < 0){
            read_count++;
            continue;
        }
        int w = 0;

        w = write(clientfifo,request,sizeof(Message));

        close(clientfifo);

        if(w < 0)
            register_op(request->rid, request->tskload, request->tskres, FAILD);
        else if(request->tskres != -1)
            register_op(request->rid, request->tskload, request->tskres, TSKDN);
        else
            register_op(request->rid, request->tskload, -1, TOOLATE);

        read_count++;
    }
     
    free(request);

    printf("[server] consumer thread stoped\n");

    pthread_exit(a);
}


int main(int argc, char** argv){

    //Initiate variables

    if(load_args(argc, argv)) return 1;

    if(setup_sigalrm()) exit(1);

    if(setup_sigpips()) exit(1);

    if(mkfifo(serverfifoname,PERM)) exit(1);

    alarm(timeout);

    circ_buffer = init_circ_buff();

    sem_init(&writer_sem,0,buffer_size);
    sem_init(&reader_sem,0,0);

	pthread_t tidd;

    //Open server fifo
    while ((serverfifo = open(serverfifoname, O_RDONLY)) < 0) {	// 1st time: keep blocking until client opens...
		perror("[server] server, open serverfifo");
		if (finish)	// server timeout!
			goto timetoclose;
	}

    while (pthread_create(&tidd, NULL, thread_consumer, NULL) != 0) {	// wait till a new thread can be created!
        perror("[server] server thread");
        usleep(10000 + (rand() % 10000));
        if (finish)	// server timeout!
            goto timetoclose;
    }

	pthread_t tid[10000];
    int count = 0;

    while(!finish || !serverfifoclosed){

        Message *request = malloc(sizeof(Message));
        int r;
        while((r = read(serverfifo,request,sizeof(Message))) <= 0){
            if(r == 0){
		        perror("[server] read serverfifo");
		        free(request);
                goto timetoclose;
            }
        }

        register_op(request->rid, request->tskload, request->tskres, RECVD);

        while (pthread_create(&tid[count], NULL, thread_producer, request) != 0) {	// wait till a new thread can be created!
            perror("[server] server thread");
            usleep(10000 + (rand() % 10000));
            if (finish)	// server timeout!
                goto timetoclose;
        }
        count ++;

    }
timetoclose:
    fprintf(stderr, "[server] stopped receiving requests\n");

    join_threads(tid, count);

    sleep(7);

    pthread_cancel(tidd);

    close(serverfifo);

    unlink(serverfifoname);

	fprintf(stderr, "[server] main terminating\n");

    free(circ_buffer);

	pthread_exit(NULL);
    return 0;
}

int load_args(int argc, char** argv){
    if(argc == 4 && !strcmp(argv[1], "-t")){
        timeout = atoi(argv[2]);

        serverfifoname = malloc(sizeof(argv[3]));
        serverfifoname = argv[3];
    }
    else if(argc == 6 && !strcmp(argv[1], "-t") && !strcmp(argv[3], "-l")){
        timeout = atoi(argv[2]);

        serverfifoname = malloc(sizeof(argv[5]));
        serverfifoname = argv[5];

        buffer_size = atoi(argv[4]);
    }
    else{
        print_usage();
        return 1;
    }

    return 0;
}

/**
 * @brief Prints program usage
 * 
 */
void print_usage(){
    fprintf(stderr,"Usage: ./s <-t nsecs> [-l bufsz] <fifoname>\n");
}

int setup_sigalrm(){

	struct sigaction new, old;
	sigset_t smask;	// signals to mask during signal handler
	sigemptyset(&smask);
	new.sa_handler = alrm;
	new.sa_mask = smask;
	new.sa_flags = 0;	// usually enough
	if(sigaction(SIGALRM, &new, &old) == -1) {
		perror ("sigaction (SIGALRM)");
		return 1;
	}

    return 0;
}

int setup_sigpips(){
    struct sigaction new2;
    sigset_t smask;
    sigemptyset(&smask);
	new2.sa_handler = pips;
	new2.sa_mask = smask;
	new2.sa_flags = 0;	// usually enough
	if(sigaction(SIGPIPE, &new2, NULL) == -1) {
		perror ("sigaction (SIGPIPE)");
		return 1;
	}
    return 0;
}

void alrm(int signo) {
	finish = 1;
	fprintf(stderr, "[server] timeout reached: %ld %ld\n", time(NULL), (unsigned long) pthread_self());
} // alrm()

void pips(int signo) {
	serverfifoclosed = 1;
	fprintf(stderr, "[client] server pipe closed\n");
} // pips()

void register_op(int i, int t, int res, enum oper oper){
    printf("%ld ; %d ; %d ; %d ; %ld ; %d", time(NULL), i, t, getpid(), pthread_self(), res);
    switch (oper)
    {
    case RECVD:
        printf(" ; RECVD \n");
        break;
    case TSKDN:
        printf(" ; TSKDN \n");
        break;
    case TSKEX:
        printf(" ; TSKEX \n");
        break;
    case TOOLATE:
        printf(" ; 2LATE \n");
        break;
    case FAILD:
        printf(" ; FAILD \n");
        break;
    default:
        break;
    }
}

circular_buff * init_circ_buff(){
    circular_buff * circ_buff = malloc(sizeof(circular_buff));
    circ_buff->buffer = malloc(buffer_size*sizeof(Message));
    circ_buff->read_index = 0;
    circ_buff->write_index = 0;
    circ_buff->length = 0;
    return circ_buff;
}

int write_to_buff(circular_buff * circ_buff, Message * message){
    sem_wait(&writer_sem);
    
    int write_index = circ_buff->write_index;
    circ_buff->buffer[write_index] = *message;

    write_index++; 
    write_index = write_index%buffer_size;
    circ_buff->write_index = write_index;

    sem_post(&reader_sem);
    fprintf(stderr,"[server] writing to buffer\n");
    return 0;
}

int read_from_buff(circular_buff * circ_buff, Message * message){
    sem_wait(&reader_sem);

    int read_index = circ_buff->read_index;

    *message = circ_buff->buffer[read_index];
    
    read_index++;
    read_index = read_index%buffer_size;
    circ_buff->read_index = read_index;

    sem_post(&writer_sem);
    fprintf(stderr,"[server] reading from buffer\n");
    return 0;
}

void join_threads(pthread_t * tid, int n){
    for(int i = 0; i < n; i++){
        pthread_join(tid[i], NULL);
    }
}

