#include <stdio.h>

int main(void) {
    int c;

    printf("输入一些字符，然后按 Ctrl+Z 回车 (Windows) 或 Ctrl+D (Linux/Mac) 结束。\n");

    while ((c = getchar()) != EOF) {
        printf("你输入的字符: '%c'  ASCII码: %d\n", c, c);
    }

    printf("检测到 EOF，程序结束。\n");
    return 0;
}
