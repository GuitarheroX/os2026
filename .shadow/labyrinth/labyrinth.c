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

int main(int argc, char *argv[]) {
    const char *short_opts = "u";
    const struct option long_opts[] = {
        {"usage", no_argument, 0, 'u'},
        {"map", required_argument, 0, 'm'},
        {"player", required_argument, 0, 'p'},
        {"move", required_argument, 1000},
        {"version", no_argument, 0, 1001},
        {0, 0, 0, 0}
    };

    int c;

    while ((c = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1){
        switch (c) {
            case 'u':
                printUsage();
                return 0;
            default:
                abort();
        }
    }
    Labyrinth *labyrinth;
    loadMap(labyrinth, "./maps/map.txt");
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
    if (playerId >= '0' && playerId <= '9'){
        return true;
    }
    return false;
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file;
    file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening file.");
        return false;
    }
    char buffer[100];
    while (fgets(buffer, sizeof(buffer), file)) {
        printf("%s", buffer);
    }
    fclose(file);
    return true;
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    // TODO: Implement this function
    Position pos = {-1, -1};
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    // TODO: Implement this function
    Position pos = {-1, -1};
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    // TODO: Implement this function
    return false;
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    // TODO: Implement this function
    return false;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    // TODO: Implement this function
    return false;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    // TODO: Implement this function
}

bool isConnected(Labyrinth *labyrinth) {
    // TODO: Implement this function
    return false;
}
