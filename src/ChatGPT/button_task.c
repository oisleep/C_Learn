#include <stdio.h>
#include <stdbool.h>

void button_task(bool input) {
    static bool last = false; // 保存上一次状态

    if (input && !last) {
        // TODO: 打印 PRESSED
        printf("PRESSED\n");
    } else if (!input && last) {
        // TODO: 打印 RELEASED
        printf("RELEASED\n");
    }

    last = input; // 更新状态
}

int main(void) {
    int c;
    bool level = false;

    puts("Type 'p' for press, 'r' for release, 'q' to quit.");
    while ((c = getchar()) != EOF) {
        if (c == 'p' || c == 'P') {
            level = true;
            button_task(level);
        } else if (c == 'r' || c == 'R') {
            level = false;
            button_task(level);
        } else if (c == 'q' || c == 'Q') {
            break;
        }
    }
}