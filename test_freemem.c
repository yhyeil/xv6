#include "types.h"
#include "user.h"

int main(int argc, char *argv[]) {
    printf(1, "Free memory pages: %d\n", freemem());
    exit();
}