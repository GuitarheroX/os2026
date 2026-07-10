#include <stdio.h>
#include <mymalloc.h>
#include <string.h>


int main() {
    printf("mymalloc tests\n");
    
    dump_free_lists();
    char *src = mymalloc(33);
    dump_free_lists();
    strcpy(src, "This is a 33 bytes block");
    printf("%s\n", src);

    myfree(src);
    dump_free_lists();
    
    char *b = mymalloc(32);
    dump_free_lists();
    strcpy(b, "This is a 32 bytes block");
    printf("%s\n", b);

    myfree(b);
    dump_free_lists();
    return 0;
}
