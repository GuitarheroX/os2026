#include <stdio.h>
#include <string.h>

int main(){
    char *line = "545 (Relay(546)) S 544 544 544 0 -1 4194624 37 0 0 0 9 158 0 0 20 0 1 0 2154 3219456 291 18446744073709551615 1 1 0 0 0 0 65536 2147024638 65536 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
    int id, ppid;
    char comm[256], state;

    // 先找到名字字段的起止
    char *open = strchr(line, '(');
    char *close = strrchr(line, ')');
    if (!open || !close) return -1;

    // 提取名字
    size_t name_len = close - open - 1;
    memcpy(comm, open + 1, name_len);
    comm[name_len] = '\0';

    // 解析前面的 PID
    if (sscanf(line, "%d", &id) != 1) return -1;

    // 解析后面的 state 和 ppid
    char *after_close = close + 1;
    if (sscanf(after_close, " %c %d", &state, &ppid) != 2) return -1;

    printf("id = %d\n", id);
    printf("comm = %s\n", comm);
    printf("state = %c\n", state);
    printf("ppid = %d\n", ppid);
    return 0;
}
