#include <stdio.h>
#include <string.h>

#define MAX_CONTACTS 100

// 定义联系人结构体
struct Contact {
    char name[50];
    char phone[20];
};

// 全局数组存放联系人
struct Contact contacts[MAX_CONTACTS];
int contact_count = 0;

// 添加联系人
void add_contact(const char *name, const char *phone) {
    if (contact_count < MAX_CONTACTS) {
        strcpy(contacts[contact_count].name, name);
        strcpy(contacts[contact_count].phone, phone);
        contact_count++;
    } else {
        printf("通讯录已满，不能再添加。\n");
    }
}

// 显示所有联系人
void show_contacts() {
    printf("---- 通讯录 ----\n");
    for (int i = 0; i < contact_count; i++) {
        printf("%d. %s - %s\n", i + 1, contacts[i].name, contacts[i].phone);
    }
}

// 按名字查找联系人
void find_contact(const char *name) {
    for (int i = 0; i < contact_count; i++) {
        if (strcmp(contacts[i].name, name) == 0) {
            printf("找到：%s 的电话是 %s\n", contacts[i].name, contacts[i].phone);
            return;
        }
    }
    printf("没有找到这个人。\n");
}

int main() {
    add_contact("Alice", "123456");
    add_contact("Bob", "9876544862145458565321525661");

    show_contacts();

    find_contact("Alice");
    find_contact("Charlie");

    return 0;
}
