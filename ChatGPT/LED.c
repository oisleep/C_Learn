#include <stdio.h>

int main(void) {
    int leds[8] = {0};  // 0=灭，1=亮

    // TODO: 用循环把 leds 全部置为 1
    for (int i = 0; i < 8; i++) {
        leds[i] = 1;
    }
    // 打印结果
    for (int i = 0; i < 8; i++) {
        printf("%d ", leds[i]);
    }
    return 0;
}
