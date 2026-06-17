#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>

int expr_num = 0;

// Compile a function definition and load it
bool compile_and_load_function(const char* function_def, const char* src) {
    // 写入 .c
    FILE *src_fp = fopen(src, "a");
    if (src_fp == NULL) {
        perror("Failed to open file");
        return false;
    }
    fprintf(src_fp, "%s", function_def);
    fclose(src_fp);
    return true;
}

// Evaluate an expression
bool evaluate_expression(const char* expression, int* result, const char *src, const char* so) {
    FILE *src_fp = fopen(src, "a");
    if (src_fp == NULL) {
        perror("Failed to open file");
        return false;
    }
    char func_name[128];
    sprintf(func_name, "__expr_wrapper_%d", expr_num++);
    fprintf(src_fp, "int %s() { return %s; }", func_name, expression);
    fclose(src_fp);

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"gcc", "-shared", "-fPIC", "-o", so, src, NULL};
        execvp("gcc", argv);
        perror("gcc");
        exit(1);
    }
    else {
        int status;
        waitpid(pid, &status, 0);

        void *handle = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
        int (*func)() = dlsym(handle, func_name);
        *result = func();
        return true;
    }

    return false;
}

int main() {
    char template[] = "/tmp/funcXXXXXX";
    int fd = mkstemp(template);
    char src[128], so[128];
    snprintf(src, sizeof(src), "%s.c", template);
    snprintf(so, sizeof(so), "%s.so", template);
    close(fd);
    unlink(template);
    
    while (true) {
        char line[128];
        printf("crepl> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        if (strncmp(line, "int", 3) == 0) {
            // 函数
            if (compile_and_load_function(line, src)) {
                printf("%s\n", "OK.");
            }
            else {
                perror("Failed to compile or load function: ");
            }
        }
        else {
            // 表达式或其他
            int result = 0;
            if (evaluate_expression(line, &result, src, so)) {
                printf("= %d\n", result);
            }
            else {
                perror("Failed to evaluate expression: ");
            }
        }
    }
}
