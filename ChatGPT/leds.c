#include <stdio.h>
#include <stdint.h>

int main(void) {
    uint8_t leds = 0x01;  // 初始: 0000 0001
    for(int i = 0; i < 8; i++){
        for(int b = 7; b >= 0; b--){
            printf("%d", (leds >> b) & 1);
        }
        printf("\n");
        leds <<= 1;  // 左移一位
    }
}