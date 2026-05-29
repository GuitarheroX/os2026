#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 1024

int main() {
    int pipefd[2];
    pid_t pid;
    char buf[BUFFER_SIZE];

    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid = fork();

    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {  // 子进程
        close(pipefd[0]);  // 关闭写端

        dup2(pipefd[1], STDOUT_FILENO);
        char *input = "Child first line\nChild second line";
        printf("%s", input);

        close(pipefd[1]);
        exit(EXIT_SUCCESS);
    } else {  // 父进程
        close(pipefd[1]);  // 关闭读端

        ssize_t n = read(pipefd[0], buf, BUFFER_SIZE);
        if (n > 0) buf[n] = '\0';
        printf("Now in father, message is\n%s\n", buf);

        close(pipefd[0]);

    }

    return 0;
}

