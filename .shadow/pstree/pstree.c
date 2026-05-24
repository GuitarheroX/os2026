#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

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
    if (sscanf(line, "%d (%255[^)]) %c %d", &id, comm, &state, &ppid) != 4) return -1;
    *ppid_out = (pid_t)ppid;
    return 0;
}

int main(void) {
    DIR *d = opendir("/proc");
    if (!d) { perror("opendir /proc"); return 1; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);

        pid_t ppid;
        if (get_ppid_from_stat(pid, &ppid) != 0) continue;

        char comm[256] = "?";
        read_comm(pid, comm, sizeof comm);
        
        Node* node = malloc(sizeof(Node));
        node->pid = pid;
        strcpy(node->comm, comm);
    }

    closedir(d);
    return 0;
}
