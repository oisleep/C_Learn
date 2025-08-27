#include <stdio.h>
#include <stdint.h>

int main(void) {
    uint8_t leds = 0x01;  // 初始: 0000 0001
    for(int i = 0; i < 8; i ++){
        for(int b = 0; b < 8; b++){
            printf("%d", (leds >> b) & 00000001);
        }
        printf("\n");
    }
}