/* TC 性能基准: 用 uptime() 计时, 输出 BENCH 行供 tools/test_perf.py 解析.
   覆盖整数循环、直接/间接调用、double 运算、数组访问与递归. */
#include <stdio.h>

int add1(int x) { return x + 1; }

int fib(int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void)
{
    int i, s, t0, t1;
    int a[64];

    t0 = uptime();
    s = 0;
    for (i = 0; i < 5000000; i++)
        s = s + i;
    t1 = uptime();
    printf("BENCH int_loop ms=%d chk=%d\n", t1 - t0,
           s == 12499997500000 ? 1 : 0);

    t0 = uptime();
    s = 0;
    for (i = 0; i < 5000000; i++)
        s = s + (2 * 3 + 4 * 5);
    t1 = uptime();
    printf("BENCH const_expr ms=%d chk=%d\n", t1 - t0, s == 130000000 ? 1 : 0);

    t0 = uptime();
    s = 0;
    for (i = 0; i < 1000000; i++)
        s = s + add1(i);
    t1 = uptime();
    printf("BENCH direct_call ms=%d chk=%d\n", t1 - t0, s > 0 ? 1 : 0);

    int (*fp)(int) = add1;
    t0 = uptime();
    s = 0;
    for (i = 0; i < 2000000; i++)
        s = s + fp(i);
    t1 = uptime();
    printf("BENCH indirect_call ms=%d chk=%d\n", t1 - t0, s > 0 ? 1 : 0);

    double d = 0.0;
    t0 = uptime();
    for (i = 0; i < 2000000; i++)
        d = d * 1.000001 + 0.5;
    t1 = uptime();
    printf("BENCH double_ops ms=%d chk=%d\n", t1 - t0, d > 0.0 ? 1 : 0);

    for (i = 0; i < 64; i++)
        a[i] = i;
    t0 = uptime();
    s = 0;
    for (i = 0; i < 5000000; i++)
        s = s + a[i & 63];
    t1 = uptime();
    printf("BENCH array_sum ms=%d chk=%d\n", t1 - t0, s > 0 ? 1 : 0);

    t0 = uptime();
    s = fib(30);
    t1 = uptime();
    printf("BENCH fib30 ms=%d chk=%d\n", t1 - t0, s == 832040 ? 1 : 0);

    return 0;
}
