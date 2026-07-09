/* fixture: simple_call_chain
 * main → foo → bar → baz */
#include <stdio.h>

void baz(void) {
    printf("baz\n");
}

void bar(void) {
    baz();
}

void foo(void) {
    bar();
}

int main(void) {
    foo();
    return 0;
}
