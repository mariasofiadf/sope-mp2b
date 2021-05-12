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
#include "common.h"
#include "lib.h"

#define PERM 0666

int timeout = 0;
int finish = 0;

char *serverfifoname = NULL;
int serverfifo = -1;		

enum oper{
    RECVD,
    TSKEX,
    TSKDN,
    TOOLATE,
    FAILD
};

void alrm(int);
int load_args(int argc, char** argv);
void print_usage();
int setup_sigalrm();
void register_op(int i, int t, int res, enum oper oper);

void * thread_producer(void* a){
    Message * request = malloc(sizeof(Message));
    request = (Message *) a;
    int res = task(request->tskload);
    request->tskres = res;
    register_op(request->tid, request->tskload, request->tskres, TSKEX);
	pthread_exit(a);
}


int main(int argc, char** argv){
    if(load_args(argc, argv)) return 1;

    if(setup_sigalrm()) exit(2);

    if(mkfifo(serverfifoname,PERM)) exit(2);

    alarm(timeout);

    while ((serverfifo = open(serverfifoname, O_RDONLY)) < 0) {	// 1st time: keep blocking until client opens...
		perror("[server] server, open serverfifo");
		if (finish)	// server timeout!
			goto timetoclose;
        printf("WHILE");
	}

	pthread_t tid;	// temporary, for any of the client threads

    while (!finish)
    {

	    Message *request = malloc(sizeof(Message));
        int r;
        while((r = read(serverfifo,request,sizeof(Message))) <= 0){
            if(r == 0){
		        perror("[server] read serverfifo");
		        free(request);
                goto timetoclose;
            }
            if(finish)
                goto timetoclose;
        }

        register_op(request->tid, request->tskload, request->tskres, RECVD);
        while (pthread_create(&tid, NULL, thread_producer, request) != 0) {	// wait till a new thread can be created!
			perror("[server] server thread");
			usleep(10000 + (rand() % 10000));
			if (finish)	// client timeout!
				goto timetoclose;
		}
        
    }
    goto timetoclose;
    
timetoclose:
    fprintf(stderr, "[server] stopped receiving requests\n");
	// strategy: 1 2
	// 1 - break all blocked threads with pthread_cancel() and assure GAVEUP messages
		// como saber os thr_ids? percorrendo /tmp/[pid].* !
	//terminate_blocked(getpid());
	// 2 - assure that all private FIFOs are removed

    close(serverfifo);

    unlink(serverfifoname);

	fprintf(stderr, "[server] main terminating\n");
	pthread_exit(NULL);
}

int load_args(int argc, char** argv){
    if(argc != 4 || strcmp(argv[1], "-t")){
        print_usage();
        return 1;
    }

    timeout = atoi(argv[2]);

    serverfifoname = malloc(sizeof(argv[3]));
    serverfifoname = argv[3];

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

void alrm(int signo) {
	finish = 1;
	fprintf(stderr, "[server] timeout reached: %ld %ld\n", time(NULL), (unsigned long) pthread_self());
} // alrm()


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
        printf("; FAILD \n");
        break;
    default:
        break;
    }
}