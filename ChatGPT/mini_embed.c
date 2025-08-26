// mini_embed.c  — PC 上的“嵌入式系统”模拟
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>

#define ARRAY_LEN(x) ((int)(sizeof(x)/sizeof((x)[0])))

// --------- 固定宽度 & 全局“硬件寄存器” ---------
static volatile uint32_t g_ms = 0;          // 1ms 系统时间
static volatile uint8_t  LED_PORT = 0x00;   // 8 位 LED 端口（1=亮，0=灭）

// --------- Tick: 1ms “中断”模拟（轮询实现）---------
static void host_sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms/1000, .tv_nsec = (ms%1000)*1000000L};
    nanosleep(&ts, NULL);
}
static void systick_poll_once(void) {
    host_sleep_ms(1);
    g_ms++;
}

// --------- 键 / 去抖 ---------
typedef enum { BTN_IDLE=0, BTN_DEBOUNCE, BTN_PRESSED, BTN_LONG } BtnState;
typedef struct {
    BtnState st;
    uint32_t last_change_ms;
    uint32_t press_ms;
    bool stable_level;   // 去抖后的稳定电平（false=未按，true=按下）
} Button;

#define DEBOUNCE_MS 10
#define LONG_MS     700

// 从标准输入读取按键事件：按下=‘p’，释放=‘r’。无输入则返回 false。
static bool read_key_event(bool *level_out) {
    // 非阻塞拉取：用 stdin 的行缓冲简单模拟
    // 建议你每隔若干 ms 敲 ‘p’/‘r’ 回车试试
    int c = fgetc(stdin);
    if (c == EOF) return false;
    if (c == 'p' || c == 'P') { *level_out = true;  return true; }
    if (c == 'r' || c == 'R') { *level_out = false; return true; }
    return false;
}

// TODO(1): 完成去抖状态机（10ms 窗口，短/长按判定）
// 需求：
// - 输入：原始电平 input_level（抖动），当前时间 now_ms
// - 维护：Button 内部状态，更新 stable_level
// - 输出：返回两个边沿事件标志（通过指针）：short_pressed、long_pressed（释放瞬间给 short；进入 LONG 状态瞬间给 long）
//
// 提示：
// - 当检测到电平变化，进入 DEBOUNCE，并记录 last_change_ms；稳定 DEBOUNCE_MS 后才认可变化
// - 认可按下瞬间记录 press_ms；持续按下超过 LONG_MS 触发 long_pressed（只触发一次），并转入 LONG
// - 释放时，如果未到 LONG_MS，则 short_pressed = true
static void button_update(Button *b, bool input_level, uint32_t now_ms,
                          bool *short_pressed, bool *long_pressed) {
    *short_pressed = false; *long_pressed = false;

    switch (b->st) {
    case BTN_IDLE:
        if (input_level != b->stable_level) { b->st = BTN_DEBOUNCE; b->last_change_ms = now_ms; }
        break;
    case BTN_DEBOUNCE:
        // TODO(1a): 如果稳定超过 DEBOUNCE_MS，则确认变化，更新 stable_level；
        // 若变为按下：st->PRESSED, press_ms = now_ms
        // 若变为释放：st->IDLE（这一步通常发生在从 PRESSED/LONG 回来时）
        // 若未到时间窗口，仍保持 DEBOUNCE
        break;
    case BTN_PRESSED:
        // TODO(1b): 如果持续按下超过 LONG_MS，触发 *long_pressed = true; st->LONG
        // 若检测到原始电平变化，进入 DEBOUNCE
        break;
    case BTN_LONG:
        // TODO(1c): 长按期间，直到释放才会变化：若电平变化，进入 DEBOUNCE
        break;
    }
}

// --------- 软 PWM（占空比 0..100） ---------
typedef struct {
    uint8_t duty;      // 目标占空比
    uint8_t counter;   // 0..99 递增
} SoftPWM;

static void pwm_init(SoftPWM *p){ p->duty = 50; p->counter = 0; }
// 每 1ms 调一次：counter 0..99 循环，返回当前应“点亮”的门限判定
static bool pwm_tick_and_is_on(SoftPWM *p){
    bool on = (p->counter < p->duty);
    p->counter = (p->counter + 1) % 100;
    return on;
}

// --------- 灯效状态机 ---------
typedef enum { FX_STEADY=0, FX_BREATH, FX_CHASER } EffectMode;
typedef struct {
    EffectMode mode;
    uint32_t   last_ms;
    uint8_t    chaser_pos; // 0..7
    int        breath_dir; // +1 / -1
} Effects;

static void effects_init(Effects *e){ e->mode=FX_STEADY; e->last_ms=0; e->chaser_pos=0; e->breath_dir=+1; }

// TODO(2): 完成特效更新（非阻塞）
// - FX_STEADY: 不变
// - FX_BREATH: 每 15ms 改变一次占空比 duty += breath_dir；到 0 或 100 反向
// - FX_CHASER: 每 80ms chaser_pos = (pos+1)%8
static void effects_update(Effects *e, SoftPWM *pwm, uint32_t now_ms){
    switch (e->mode){
    case FX_STEADY: break;
    case FX_BREATH:
        // TODO(2a)
        break;
    case FX_CHASER:
        // TODO(2b)
        break;
    }
}

// --------- 协作式任务 ---------
typedef struct {
    Button  btn;
    SoftPWM pwm;
    Effects fx;
    uint8_t brightness_idx; // 0..3  -> 25/50/75/100
} App;

static const uint8_t k_brightness_table[4] = {25, 50, 75, 100};

static void app_init(App *a){
    a->btn = (Button){ .st=BTN_IDLE, .last_change_ms=0, .press_ms=0, .stable_level=false };
    pwm_init(&a->pwm);
    effects_init(&a->fx);
    a->brightness_idx = 1; // 50%
    a->pwm.duty = k_brightness_table[a->brightness_idx];
}

// TODO(3): 按键处理任务（1ms 调用一次）
// - 短按：切换模式 -> STEADY -> BREATH -> CHASER -> STEADY
// - 长按：切换亮度档 25/50/75/100（更新 pwm.duty）
// - 使用 button_update 返回的 short_pressed/long_pressed
static void task_buttons(App *a, uint32_t now_ms){
    static bool raw_level = false;
    bool short_p=false, long_p=false;

    // 从 stdin 抓事件（可多次触发，最后一次为准）
    bool lvl;
    if (read_key_event(&lvl)) raw_level = lvl;

    // TODO(3a): 调用 button_update，基于事件切换 a->fx.mode 和 a->brightness_idx / pwm.duty
    (void)now_ms;
}

// TODO(4): 渲染任务（每 10ms 调用一次即可）
// - 计算当前 8 位 LED 显示：
//   * STEADY/BREATH：全部位等于 pwm_on ? 1 : 0
//   * CHASER：只有 chaser_pos 那一位等于 pwm_on，其它位为 0
// - 打印一行 8 字符（1/0），不阻塞
static void task_render(App *a){
    bool on = pwm_tick_and_is_on(&a->pwm);
    uint8_t port = 0;

    switch (a->fx.mode){
    case FX_STEADY: case FX_BREATH:
        // TODO(4a): port = on ? 0xFF : 0x00;
        port = on ? 0xFF : 0x00;
        break;
    case FX_CHASER:
        // TODO(4b): 设置第 fx.chaser_pos 位根据 on 决定亮灭
        port = on ? (uint8_t)(1u << a->fx.chaser_pos) : 0u;
        break;
    }

    LED_PORT = port;
    // 打印
    for (int i=7;i>=0;--i) putchar( (LED_PORT>>i)&1 ? '1':'0');
    putchar('\n');
}

// 主循环：1ms tick，分频调任务
int main(void){
    App app; app_init(&app);
    uint32_t last_render = 0;

    // 关闭 stdin 缓冲（便于即刻读到 p/r）
    setbuf(stdin, NULL);

    while (g_ms < 3600000) { // 最多跑一小时
        systick_poll_once();

        // 1ms 任务
        task_buttons(&app, g_ms);
        effects_update(&app.fx, &app.pwm, g_ms);

        // 10ms 渲染
        if (g_ms - last_render >= 10){
            last_render = g_ms;
            task_render(&app);
        }
    }
    return 0;
}
