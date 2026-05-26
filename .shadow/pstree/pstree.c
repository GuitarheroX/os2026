#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

typedef struct Node {
    pid_t pid;
    pid_t ppid;
    char comm[256];
    struct Node* children;
    struct Node* next;
} Node;

typedef struct HashEntry {
    pid_t pid;
    Node* node;
    struct HashEntry* next;
} HashEntry;

HashEntry *hash_table[1024];

unsigned int hash(pid_t pid) {
    return pid % 1024;
}

void insert(pid_t pid, Node* node){
    HashEntry *e = malloc(sizeof(HashEntry));
    e->pid = pid;
    e->node = node;
    e->next = hash_table[hash(pid)];
    hash_table[hash(pid)] = e;
}

Node* hashFind(pid_t pid) {
    for (HashEntry *e = hash_table[hash(pid)]; e; e = e->next) {
        if (e->pid == pid) {
            return e->node;
        }
    }
    return NULL;
}

static int read_comm(pid_t pid, char *buf, size_t n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)n, f)) { fclose(f); return -1; }
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 0;
}

static int get_ppid_from_stat(pid_t pid, pid_t *ppid_out) {
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    // 将文件内容读取到 line 中
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);

    int id, ppid;
    char comm[256], state;

    char *open = strchr(line, '(');
    char *close = strrchr(line, ')');
    if (!open || !close) return -1;
    size_t name_len = close - open - 1;
    memcpy(comm, open + 1, name_len);
    comm[name_len] = '\0';

    if (sscanf(line, "%d", &id) != 1) return -1;

    char *after_close = close + 1;
    if (sscanf(after_close, " %c %d", &state, &ppid) != 2) return -1;

    *ppid_out = (pid_t)ppid;
    return 0;
}

int cmp_pid(const void*a, const void*b) {
    pid_t pa = *(const pid_t *)a;
    pid_t pb = *(const pid_t *)b;
    return (pa < pb) - (pa > pb);
}

void print_tree(Node* node, const char* prefix, int is_last, int show_pids_flag) {
    if (!node) return;

    printf("%s", prefix);
    printf(is_last ? "└─" : "├─");
    printf("%s", node->comm);
    if (show_pids_flag) {
        printf("(%d)", node->pid);
    }
    printf("\n");

    char new_prefix[256];
    snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, is_last ? "    " : "│   ");

    print_tree(node->children, new_prefix, 1, show_pids_flag);
    print_tree(node->next, prefix, is_last, show_pids_flag);
}

int main(int argc, char *argv[]) {
    const char *short_opts = "pnV";
    const struct option long_opts[] = {
        {"show-pids", no_argument, 0, 'p'},
        {"numeric-sort", no_argument, 0, 'n'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };
    int arg = 0;
    int show_pids_flag = 0;
    int numeric_sort_flag = 0;
    int version_flag = 0;
    while ((arg = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
        switch (arg) {
            case 'p':
                show_pids_flag = 1;
                break;
            case 'n':
                numeric_sort_flag = 1;
                break;
            case 'V':
                version_flag = 1;
                break;
            default:
                fprintf(stderr, "Error: nonexist arg\n");
                return 1;
        }
    }
    if (version_flag) {
        if (show_pids_flag || numeric_sort_flag) {
            fprintf(stderr, "Error: --version cannot be used with other options\n");
            return 1;
        }
        else if (optind < argc) {
            fprintf(stderr, "Error: --version does not accept arguments\n");
            return 1;
        }
        printf("pstree, Version 1.0\n");
        return 0;
    }

    DIR *d = opendir("/proc");
    if (!d) { perror("opendir /proc"); return 1; }
    pid_t all_pids[1024]; 
    int num_pid = 0;
    struct dirent *de;
    // 储存进哈希表
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);

        pid_t ppid;
        if (get_ppid_from_stat(pid, &ppid) != 0) continue;
        all_pids[num_pid] = pid;
        num_pid += 1;
        char comm[256] = "?";
        read_comm(pid, comm, sizeof comm);
        
        Node* node = malloc(sizeof(Node));
        node->pid = pid;
        node->ppid = ppid;
        strcpy(node->comm, comm);
        insert(pid, node);
    }

    // 形成树结构
    if (numeric_sort_flag) { 
        qsort(all_pids, num_pid, sizeof(pid_t), cmp_pid);
    }
    Node* root = NULL;
    for (int i = 0; i < num_pid; i++) {
        Node* node = hashFind(all_pids[i]);
        if (node->ppid == 0) {
            root = node; 
            continue;
        }
        Node* father_node = hashFind(node->ppid);
        if (!father_node) father_node = root;
        if (father_node->children) {
            node->next = father_node->children;
        }
        father_node->children = node;
    }
    closedir(d);

    // 打印进程树
    char *prefix = "";
    int is_last = 0;
    print_tree(root, prefix, is_last, show_pids_flag); 
    return 0;
}
