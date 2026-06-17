#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>

int wrapper_count = 0;

int main(int argc, char *argv[]) {
    static char line[4096];
    char cmd[4096];
    char temp_so_path[] = "/tmp/crepl_so.XXXXXX";
    char temp_src_path[] = "/tmp/crepl_src.XXXXXX";

    // 分配临时文件
    int fd = mkstemp(temp_so_path);
    if (fd == -1) {
        perror("mkstemp failed");
        return EXIT_FAILURE;
    }
    fd = mkstemp(temp_src_path);
    if (fd == -1) {
        perror("mkstemp failed");
        return EXIT_FAILURE;
    }

    // -Wno-implicit-function-declaration   
    // 用于绕过在调用其他函数场景下的隐式函数定义的检查
    sprintf(cmd,
            "gcc -Wno-implicit-function-declaration -xc "
            "-shared -o %s %s",
            temp_so_path, temp_src_path);

    while (1) {
        printf("crepl> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        // To be implemented.
        // printf("Got %zu chars.\n", strlen(line));

        int is_function = strncmp(line, "int ", strlen("int ")) == 0;
        char func_name[256];
        char func[4096];

        if (is_function) {
            strcpy(func, line);
        } else {
            sprintf(func_name, "__expr_wrapper_%d", wrapper_count++);
            sprintf(func, "int %s() { return %s; }", func_name, line);
        }

        // 将函数写入临时文件
        // 如果是表达式，就编译成 so 然后 dlopen 执行 expr wrapper
        FILE* file = fopen(temp_src_path, "a");
        if (file == NULL) {
            perror("Failed to open file");
            return EXIT_FAILURE;
        }
        fprintf(file, "%s\n", func);
        fflush(file);
        fclose(file);

        // 只是添加函数定义的话不需要编译
        if (is_function) {
            printf("OK.\n");
            continue;
        }

        // 编译成 so
        system(cmd);

        void *handle;
        int (*function)(void);
        char *error;
        int eval_result;

        // 加载共享库
        handle = dlopen(temp_so_path, RTLD_LAZY);
        if (!handle) {
            fprintf(stderr, "%s\n", dlerror());
            return 1;
        }

        // 清除现有的错误
        dlerror();
        
        *(void **) (&function) = dlsym(handle, func_name);
        if ((error = dlerror()) != NULL)  {
            fprintf(stderr, "%s\n", error);
            dlclose(handle);
            return 1;
        }

        // 调用函数
        eval_result = function();

        // 关闭共享库
        dlclose(handle);

        printf("= %d.\n", eval_result);
    }
}
