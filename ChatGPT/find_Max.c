#include <stdio.h>

int findMax(int arr[], int len){
    int max = arr[0];
    for(int i = 0; i < len; i++){
        if (arr[i] > max){
            max = arr[i];
        }
    }
    return max;
}

int main(void) {
    int numbers[] = {3, 5, 7, 2, 8, -1, 4};
    int len = sizeof(numbers)/sizeof(numbers[0]);
    int max_value = findMax(numbers, len);
    printf("The maximum value is: %d\n", max_value);
    return 0;
}