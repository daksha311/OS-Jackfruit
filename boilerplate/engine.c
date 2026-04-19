#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#define STACK_SIZE (1024*1024)
#define SOCK_PATH "/tmp/mini_runtime.sock"
#define MAX 10

typedef struct {
    char id[32];
    pid_t pid;
    int running;
} container;

container containers[MAX];
int count = 0;

/* ================= BOUNDED BUFFER ================= */

#define BUF_SIZE 16
char logbuf[BUF_SIZE][256];
int head=0, tail=0, count_buf=0;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

void push(char *msg) {
    pthread_mutex_lock(&mtx);
    while(count_buf == BUF_SIZE)
        pthread_cond_wait(&not_full, &mtx);

    strcpy(logbuf[tail], msg);
    tail = (tail+1)%BUF_SIZE;
    count_buf++;

    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&mtx);
}

void pop(char *out) {
    pthread_mutex_lock(&mtx);
    while(count_buf == 0)
        pthread_cond_wait(&not_empty, &mtx);

    strcpy(out, logbuf[head]);
    head = (head+1)%BUF_SIZE;
    count_buf--;

    pthread_cond_signal(&not_full);
    pthread_mutex_unlock(&mtx);
}

void* logger(void* arg) {
    char msg[256];
    while(1) {
        pop(msg);
        printf("[LOG] %s\n", msg);
    }
}

/* ================= CONTAINER ================= */

int child_func(void *arg) {
    char **args = (char**)arg;

    sethostname("container", 9);
    chroot(args[0]);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    execl("/bin/sh","sh",NULL);
    perror("exec");
    return 1;
}

void start_container(char *id, char *rootfs) {
    char *stack = malloc(STACK_SIZE);
    char *stack_top = stack + STACK_SIZE;

    char *args[] = {rootfs, NULL};

    pid_t pid = clone(child_func, stack_top,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        args);

    containers[count].pid = pid;
    strcpy(containers[count].id, id);
    containers[count].running = 1;
    count++;

    char msg[128];
    sprintf(msg,"Started %s PID=%d", id, pid);
    push(msg);
}

/* ================= SUPERVISOR ================= */

void handle_sigchld(int sig) {
    int status;
    pid_t pid;
    while((pid = waitpid(-1,&status,WNOHANG)) > 0) {
        for(int i=0;i<count;i++){
            if(containers[i].pid == pid){
                containers[i].running = 0;
                printf("[Supervisor] %s stopped\n", containers[i].id);
            }
        }
    }
}

void supervisor() {
    int server_fd, client_fd;
    struct sockaddr_un addr;

    signal(SIGCHLD, handle_sigchld);

    pthread_t t;
    pthread_create(&t,NULL,logger,NULL);

    unlink(SOCK_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(server_fd,(struct sockaddr*)&addr,sizeof(addr));
    listen(server_fd,5);

    printf("Supervisor running...\n");

    while(1){
        client_fd = accept(server_fd,NULL,NULL);

        char buf[256];
        read(client_fd,buf,sizeof(buf));

        if(strncmp(buf,"START",5)==0){
            char id[32], rootfs[128];
            sscanf(buf,"START %s %s",id,rootfs);
            start_container(id,rootfs);
            write(client_fd,"OK\n",3);
        }
        else if(strncmp(buf,"PS",2)==0){
            char out[512]="";
            for(int i=0;i<count;i++){
                char line[128];
                sprintf(line,"ID:%s PID:%d %s\n",
                    containers[i].id,
                    containers[i].pid,
                    containers[i].running?"RUNNING":"STOPPED");
                strcat(out,line);
            }
            write(client_fd,out,strlen(out));
        }

        close(client_fd);
    }
}

/* ================= CLIENT ================= */

void send_cmd(char *cmd) {
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX,SOCK_STREAM,0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path,SOCK_PATH);

    connect(sock,(struct sockaddr*)&addr,sizeof(addr));

    write(sock,cmd,strlen(cmd));

    char buf[512];
    int n = read(sock,buf,sizeof(buf));
    buf[n]=0;
    printf("%s",buf);

    close(sock);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {

    if(argc < 2){
        printf("Usage\n");
        return 1;
    }

    if(strcmp(argv[1],"supervisor")==0){
        supervisor();
    }
    else if(strcmp(argv[1],"start")==0){
        char cmd[256];
        sprintf(cmd,"START %s %s",argv[2],argv[3]);
        send_cmd(cmd);
    }
    else if(strcmp(argv[1],"ps")==0){
        send_cmd("PS");
    }
}
