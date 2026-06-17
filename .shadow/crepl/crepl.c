#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>

int expr_num = 0;

// Compile a function definition and load it
bool compile_and_load_function(const char* function_def) {
    // 生成 .c .so 文件的名字
    char template[] = "/tmp/funcXXXXXX";
    int fd = mkstemp(template);
    char src[128], so[128];
    snprintf(src, sizeof(src), "%s.c", template);
    snprintf(so, sizeof(so), "%s.so", template);
    close(fd);
    unlink(template);

    // 写入 .c
    FILE *src_fp = fopen(src, "w");
    fprintf(src_fp, "%s", function_def);
    fclose(src_fp);
    
    pid_t pid = fork();
    if (pid == 0) {
        // 编译为 .so
        char *argv[] = {"gcc", "-shared", "-fPIC", "-o", so, src, NULL};
        execvp("gcc", argv);
        perror("gcc");
        exit(1);
    }
    else {
       int status;
       waitpid(pid, &status, 0);

       void *_ = dlopen(so, RTLD_GLOBAL);
       return true;
    }
    return false;
}

// Evaluate an expression
bool evaluate_expression(const char* expression, int* result) {
    char template[] = "/tmp/exprXXXXXX";
    int fd = mkstemp(template);
    char src[128], so[128];
    snprintf(src, sizeof(src), "%s.c", template);
    snprintf(so, sizeof(so), "%s.so", template);
    close(fd);
    unlink(template);

    FILE *src_fp = fopen(src, "w");
    char func_name[128] = "__expr_wrapper_";
    snprintf(func_name + strlen(func_name), sizeof(func_name) - strlen(func_name), "%d", expr_num);
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

        void *handle = dlopen(so, RTLD_NOW);
        int (*func)() = dlsym(handle, func_name);
        *result = func();
        return true;
    }

    return false;
}

int main() {
    while (true) {
        char line[128];
        printf("crepl> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        if (strncmp(line, "int", 3) == 0) {
            // 函数
            if (compile_and_load_function(line)) {
                printf("%s\n", "OK.");
            }
            else {
                perror("Failed to compile or load function: ");
            }
        }
        else {
            // 表达式或其他
            int result = 0;
            if (evaluate_expression(line, &result)) {
                printf("= %d\n", result);
            }
            else {
                perror("Failed to evaluate expression: ");
            }
        }
    }
}
