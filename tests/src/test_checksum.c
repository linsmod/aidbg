#include <stdio.h>

int cs_global = 42;

int cs_add(int a, int b)
{
    int result = a + b;
    return result;
}

int main(void)
{
    int z = cs_add(cs_global, 1);
    printf("z = %d\n", z);
    return 0;
}
