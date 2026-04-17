/* demo_tree.c – tiny program that writes a tree object from the index */
#include "pes.h"
#include "tree.h"
#include "index.h"
#include <stdio.h>

int main(void) {
    /* clean the repo first – the same steps test_tree does */
    system("rm -rf .pes");
    system("mkdir -p .pes/objects .pes/refs/heads");

    /* Stage a couple of files so the index isn’t empty */
    system("echo 'hello' > a.txt");
    system("echo 'world' > b.txt");
    // add them to the index (uses index_add from index.c)
    // you could also call the library function directly, but a shell call is fine:
    system("./pes add a.txt");
    system("./pes add b.txt");

    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0) {
        fprintf(stderr, "tree_from_index failed\n");
        return 1;
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&tree_id, hex);
    printf("Tree object written, id = %s\n", hex);
    return 0;
}

