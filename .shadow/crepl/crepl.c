#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>

int expr_num = 0;

// Compile a function definition and load it
bool compile_and_load_function(const char* function_def) {
    char template[] = "/tmp/funcXXXXXX";
    int fd = mkstemp(template);
    fprintf(fd, function_def);
    fclose(fd);
    
    char name[128];
    snprintf(name, sizeof(name), "%s.so", template);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"gcc", "-shared", "-fPIC", "-o", name, template, NULL};
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
    char func_name = "__expr_wrapper_";
    snprintf(func_name + str(func_name), sizeof(func_name) - strlen(func_name), "%d", expr_num);
    char template[] = "/tmp/exprXXXXXX";
    int fd = mkstemp(template);
    fprintf(fd, "int %s() { return %s; }", func_name, expression);
    fclose(fd);

    char name[128];
    snprintf(name, sizeof(name), "%s.so", template);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"gcc", "-shared", "-fPIC", "-o", name, template, NULL};
        execvp("gcc", argv);
        perror("gcc");
        exit(1);
    }
    else {
        int status;
        waitpid(pid, &status, 0);

        void *handle = dlopen(name);
        void (*func)() = dlsys(handle, func_name);
        *result = func();
        return true;
    }

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
