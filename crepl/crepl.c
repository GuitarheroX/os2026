#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>

// Compile a function definition and load it
bool compile_and_load_function(const char* function_def) {
    FILE *fp = fopen("/tmp/foo.c", "w");
    fprintf(fp, function_def);
    fclose(fp);
    
    char *name = "/tmp/foo.so";
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"gcc", "-shared", "-fPIC", "-o", name, "/tmp/foo.c", NULL};
        execvp("gcc", argv);
        perror("gcc");
        exit(1);
    }
    else {
       int status;
       waitpid(pid, &status, 0);

       // load
       void *handle = dlopen(name, RTLD_GLOBAL);
       return true;
    }
    return false;
}

// Evaluate an expression
bool evaluate_expression(const char* expression, int* result) {
    return false;
}

int main() {
    while (True) {
        printf("crepl> ");
        fflush(stdout);
        fgets(line, sizeof(line), stdin);

        if (strncmp(str, "int", 3) == 0) {
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
