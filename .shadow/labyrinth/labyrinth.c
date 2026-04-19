#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <testkit.h>
#include <unistd.h>
#include <getopt.h>
#include "labyrinth.h"

void printUsage();

void printMap(Labyrinth *labyrinth);

Position allocatePlayer(Labyrinth *labyrinth, char playerId);


int main(int argc, char *argv[]) {
    const char *short_opts = "m:p:";
    const struct option long_opts[] = {
        {"map", required_argument, 0, 'm'},
        {"player", required_argument, 0, 'p'},
        {"move", required_argument, 0, 1000},
        {"version", no_argument, 0, 1001},
        {0, 0, 0, 0}
    };

    int arg = 0;
    char *map_file = NULL;
    char *playerId = NULL;
    const char *direction = NULL;
    int version_flag = 0;
    while ((arg = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1){
        switch (arg) {
            case 'm':
                map_file = optarg;
                break;
            case 'p':
                playerId = optarg;
                break;
            case 1000:
                direction = optarg;
                break;
            case 1001:
                version_flag = 1;
                break;
            default:
                fprintf(stderr, "Error: nonexist arg\n");
                return 1;
        }
    }
    if (version_flag) {
        if (map_file || playerId || direction) {
            fprintf(stderr, "Error: --version cannot be used with other options\n");
            return 1;
        }
        else if (optind < argc) {
            fprintf(stderr, "Error: --version does not accept arguments\n");
            return 1;
        }
        printf("Labyrinth Game, Version 1.0\n");
        return 0;
    }
    
    if (!map_file || !playerId) {
        fprintf(stderr, "Error: --map and --player must be used together\n");
        printUsage();
        return 1;
    }

    Labyrinth labyrinth = {0};
    if (!loadMap(&labyrinth, map_file) || !isValidPlayer(*playerId) || !isConnected(&labyrinth)) {
        return 1;
    }
    
    if (direction) {
        if(!movePlayer(&labyrinth, *playerId, direction)) {
            return 1;
        }
    }
    printMap(&labyrinth);
    saveMap(&labyrinth, map_file);
    return 0;
}

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    if (playerId < '0' || playerId > '9'){
        fprintf(stderr, "Error: Invalid playerId %c, expected 0 to 9\n", playerId);
        return false;
    }
    return true;
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: opening file %s\n", filename);
        return false;
    }
    int ch;
    int r = 0;
    int c = 0;
    while ((ch = fgetc(file)) != EOF) {
        if (r >= MAX_ROWS) {
            fprintf(stderr, "Error: map exceeds maximum rows (%d)\n", MAX_ROWS);
            fclose(file);
            return false;
        }
        if (ch == '\n') {
            if (r == 0) {
                printf("1");
                labyrinth->cols = c;
            }
            if (c != labyrinth->cols) {
                fprintf(stderr, "Error: inconsistent rows\n");
                return false;
            }
            r++;
            c = 0;
        }
        else {
            if (c >= MAX_COLS) {
                fprintf(stderr, "Error: map exceeds maximum cols (%d)\n", MAX_COLS);
                fclose(file);
                return false;
            }
            labyrinth->map[r][c++] = ch;
        }
    }
    labyrinth->rows = r;
    fclose(file);
    return true;
}

void printMap(Labyrinth *labyrinth) {
    for (int i = 0; i < labyrinth->rows; i++){
        for (int j = 0; j < labyrinth->cols; j++){
            printf("%c", labyrinth->map[i][j]);
        }
        printf("\n");
    }
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    Position pos = {-1, -1};
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++){
            if (labyrinth->map[i][j] == playerId) {
                pos.row = i;
                pos.col = j;
            }
        }
    }
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    Position pos = {-1, -1};
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++){
            if (isEmptySpace(labyrinth, i, j)) {
                pos.row = i;
                pos.col = j;
                goto found;
            }
        }
    }
    found:
    return pos;
}

Position allocatePlayer(Labyrinth *labyrinth, char playerId) {
    Position pos = findPlayer(labyrinth, playerId);
    if (pos.row != -1) {
        return pos;
    }
    pos = findFirstEmptySpace(labyrinth);
    if (pos.row == -1) {
        fprintf(stderr, "No space for a new player");
    }
    else {
        labyrinth->map[pos.row][pos.col] = playerId;
    }
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    if (row >= labyrinth->rows && col >= labyrinth->cols) {
        return false;
    }
    if (labyrinth->map[row][col] == '.') {
        return true;
    }
    return false;
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    Position pos = allocatePlayer(labyrinth, playerId);
    int i = 0;
    int j = 0;
    if (strcmp(direction, "up") == 0) {
        i = -1;
        j = 0;
    }
    else if (strcmp(direction, "down") == 0) {
        i = 1;
        j = 0;
    }
    else if (strcmp(direction, "left") == 0) {
        i = 0;
        j = -1;
    }
    else if (strcmp(direction, "right") == 0) {
        i = 0;
        j = 1;
    }
    else {
        fprintf(stderr, "Error: unknown direction %s\n", direction);
    }
    int new_row = pos.row + i;
    int new_col = pos.col + j;
    if (!isEmptySpace(labyrinth, new_row, new_col)) {
        fprintf(stderr, "Error: position (%d, %d) is not empty space\n", new_row, new_col);
        return false;
    }
    labyrinth->map[pos.row][pos.col] = '.';
    labyrinth->map[new_row][new_col] = playerId;
    return true;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("fopen");
        return false;
    }

    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++) {
            fputc(labyrinth->map[i][j], fp);
        }
        fputc('\n', fp);
    }

    fclose(fp);
    return false;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    visited[row][col] = true;
    int dr[] = {-1, 1, 0, 0};
    int dc[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int new_row = row + dr[i];
        int new_col = col + dc[i];
        if (new_row >= 0 && new_row < labyrinth->rows &&
            new_col >= 0 && new_col < labyrinth->cols &&
            (labyrinth->map[new_row][new_col] != '#') && !visited[new_row][new_col]) {
            dfs(labyrinth, new_row, new_col, visited);    
        }
    }
}

bool isConnected(Labyrinth *labyrinth) {
    bool visited[MAX_ROWS][MAX_COLS] = {false};
    Position pos = {-1, -1};
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++){
            if (labyrinth->map[i][j] != '#') {
                pos.row = i;
                pos.col = j;
                goto found;
            }
        }
    }
    found:
    if (pos.row == -1) {
        fprintf(stderr, "Error: no empty space\n");
        return false;
    }
    dfs(labyrinth, pos.row, pos.col, visited);
    for (int i = 0; i < labyrinth->rows; i++) {
        for (int j = 0; j < labyrinth->cols; j++){
            if ((labyrinth->map[i][j] != '#') && !visited[i][j]) {
                fprintf(stderr, "Error: unreachable space (%d, %d)\n", i, j);
                return false;
            }
        }
    }
    return true;
}
