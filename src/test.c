/* Logic tests for the long-press state machine (runs on any PC). */
#define TEST
#include "remote-sleep.c"
#include <assert.h>

int main(void)
{
    struct machine m = { 0 };
    assert(HOLD_MS == 1000 && is_prev(KEY_UP) && !is_prev(KEY_DOWN));

    /* short click on previous -> click on release */
    assert(machine_key(&m, KEY_UP, 1, 100) == ACT_NONE);
    assert(machine_timeout(&m, 150) == 950);
    assert(machine_tick(&m, 150) == ACT_NONE);
    assert(machine_key(&m, KEY_UP, 0, 200) == ACT_CLICK);
    assert(machine_timeout(&m, 200) == -1);

    /* next -> click on release, never sleeps however long it is held */
    assert(machine_key(&m, KEY_DOWN, 1, 300) == ACT_NONE);
    assert(machine_tick(&m, 5000) == ACT_NONE);
    assert(machine_key(&m, KEY_DOWN, 0, 5000) == ACT_CLICK);

    /* long press: sleep fires once via timer; repeats, release, strays swallowed */
    memset(&m, 0, sizeof m);
    assert(machine_key(&m, KEY_UP, 1, 10000) == ACT_NONE);
    assert(machine_key(&m, KEY_UP, 2, 10300) == ACT_NONE);
    assert(machine_tick(&m, 10999) == ACT_NONE);
    assert(machine_timeout(&m, 10999) == 1);
    assert(machine_tick(&m, 11000) == ACT_SLEEP);
    assert(machine_tick(&m, 11050) == ACT_NONE);
    assert(machine_timeout(&m, 11050) == -1);
    assert(machine_key(&m, KEY_UP, 0, 11400) == ACT_NONE);
    assert(machine_key(&m, KEY_DOWN, 1, 11500) == ACT_NONE);
    assert(machine_key(&m, KEY_DOWN, 0, 11600) == ACT_NONE);
    /* back to normal afterwards */
    assert(machine_key(&m, KEY_UP, 1, 20000) == ACT_NONE);
    assert(machine_key(&m, KEY_UP, 0, 20100) == ACT_CLICK);

    /* release arrives before the timer ran */
    memset(&m, 0, sizeof m);
    machine_key(&m, KEY_UP, 1, 0);
    assert(machine_key(&m, KEY_UP, 0, 1200) == ACT_SLEEP);
    assert(machine_tick(&m, 1201) == ACT_NONE);

    /* release lost while asleep, next press starts clean */
    memset(&m, 0, sizeof m);
    machine_key(&m, KEY_UP, 1, 0);
    assert(machine_tick(&m, 1000) == ACT_SLEEP);
    assert(machine_key(&m, KEY_UP, 1, 60000) == ACT_NONE);
    assert(machine_tick(&m, 60500) == ACT_NONE);
    assert(machine_key(&m, KEY_UP, 0, 60600) == ACT_CLICK);

    /* fast back-paging never sleeps */
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 10; i++) {
        long long t0 = 1000 + i * 250;
        assert(machine_key(&m, KEY_UP, 1, t0) == ACT_NONE);
        assert(machine_key(&m, KEY_UP, 0, t0 + 150) == ACT_CLICK);
    }

    puts("all tests passed");
    return 0;
}
