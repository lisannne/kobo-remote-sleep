/* Logic tests for the long-press state machine (runs on any PC). */
#define TEST
#include "remote-sleep.c"
#include <assert.h>

int main(void)
{
    struct config c;
    struct machine m;
    int code;

    config_defaults(&c);
    assert(c.hold_ms == 1000 && is_prev(&c, KEY_UP) && !is_prev(&c, KEY_DOWN));

    /* config parsing */
    char l1[] = "hold_ms = 1500  # comment", l2[] = "prev_key=108, 114", l3[] = "grab=0";
    char l4[] = "device_name= Kobo Remote ", l5[] = "hold_ms=5";
    config_parse_line(&c, l1); config_parse_line(&c, l2); config_parse_line(&c, l3);
    config_parse_line(&c, l4); config_parse_line(&c, l5);
    assert(c.hold_ms == 1500 && c.nprev == 2 && is_prev(&c, 108) && is_prev(&c, 114));
    assert(!is_prev(&c, KEY_UP) && c.grab == 0 && !strcmp(c.device_name, "Kobo Remote"));

    config_defaults(&c);

    /* short click on previous -> forwarded as click on release */
    memset(&m, 0, sizeof m);
    assert(machine_key(&m, &c, KEY_UP, 1, 100, &code) == ACT_NONE);
    assert(machine_timeout(&m, &c, 150) == 950);
    assert(machine_tick(&m, &c, 150) == ACT_NONE);
    code = 0;
    assert(machine_key(&m, &c, KEY_UP, 0, 200, &code) == ACT_CLICK && code == KEY_UP);
    assert(machine_timeout(&m, &c, 200) == -1);

    /* next button -> click on release, never sleeps however long it is held */
    assert(machine_key(&m, &c, KEY_DOWN, 1, 300, &code) == ACT_NONE);
    assert(machine_tick(&m, &c, 5000) == ACT_NONE);
    assert(machine_key(&m, &c, KEY_DOWN, 0, 5000, &code) == ACT_CLICK && code == KEY_DOWN);

    /* long press: sleep fires once via timer, repeats and release are swallowed */
    memset(&m, 0, sizeof m);
    assert(machine_key(&m, &c, KEY_UP, 1, 10000, &code) == ACT_NONE);
    assert(machine_key(&m, &c, KEY_UP, 2, 10300, &code) == ACT_NONE);
    assert(machine_tick(&m, &c, 10999) == ACT_NONE);
    assert(machine_timeout(&m, &c, 10999) == 1);
    assert(machine_tick(&m, &c, 11000) == ACT_SLEEP);
    assert(machine_tick(&m, &c, 11050) == ACT_NONE);
    assert(machine_timeout(&m, &c, 11050) == -1);
    assert(machine_key(&m, &c, KEY_UP, 0, 11400, &code) == ACT_NONE);
    /* stray press during the quiet window is ignored too */
    assert(machine_key(&m, &c, KEY_DOWN, 1, 11500, &code) == ACT_NONE);
    assert(machine_key(&m, &c, KEY_DOWN, 0, 11600, &code) == ACT_NONE);
    /* afterwards everything is back to normal */
    assert(machine_key(&m, &c, KEY_UP, 1, 20000, &code) == ACT_NONE);
    assert(machine_key(&m, &c, KEY_UP, 0, 20100, &code) == ACT_CLICK);

    /* long press where the release arrives before the timer ran */
    memset(&m, 0, sizeof m);
    machine_key(&m, &c, KEY_UP, 1, 0, &code);
    assert(machine_key(&m, &c, KEY_UP, 0, 1200, &code) == ACT_SLEEP);
    assert(machine_tick(&m, &c, 1201) == ACT_NONE);

    /* release lost (remote dropped while asleep), next press starts clean */
    memset(&m, 0, sizeof m);
    machine_key(&m, &c, KEY_UP, 1, 0, &code);
    assert(machine_tick(&m, &c, 1000) == ACT_SLEEP);
    assert(machine_key(&m, &c, KEY_UP, 1, 60000, &code) == ACT_NONE);
    assert(machine_tick(&m, &c, 60500) == ACT_NONE);
    assert(machine_key(&m, &c, KEY_UP, 0, 60600, &code) == ACT_CLICK);

    /* fast back-paging must not sleep while run detection is off (default) */
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 10; i++) {
        long long t0 = 1000 + i * 250;
        assert(machine_key(&m, &c, KEY_UP, 1, t0, &code) == ACT_NONE);
        assert(machine_tick(&m, &c, t0 + 100) == ACT_NONE);
        assert(machine_key(&m, &c, KEY_UP, 0, t0 + 150, &code) == ACT_CLICK);
    }

    /* remote that sends a stream of 150 ms clicks while held: repeat_gap_ms=250 */
    c.repeat_gap_ms = 250;
    memset(&m, 0, sizeof m);
    int slept = 0, clicks = 0;
    for (int i = 0; i < 10 && !slept; i++) {
        long long t0 = 1000 + i * 250;
        enum action a = machine_key(&m, &c, KEY_UP, 1, t0, &code);
        if (a == ACT_SLEEP) { slept = 1; assert(t0 - 1000 >= 1000); break; }
        a = machine_key(&m, &c, KEY_UP, 0, t0 + 150, &code);
        if (a == ACT_SLEEP) { slept = 1; assert(t0 + 150 - 1000 >= 1000); break; }
        assert(a == ACT_CLICK); clicks++;
    }
    assert(slept && clicks == 4);
    /* slow clicks (gap > repeat_gap_ms) never add up to a long press */
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 10; i++) {
        long long t0 = 1000 + i * 500;
        assert(machine_key(&m, &c, KEY_UP, 1, t0, &code) == ACT_NONE);
        assert(machine_key(&m, &c, KEY_UP, 0, t0 + 150, &code) == ACT_CLICK);
    }
    /* next button in between breaks a run */
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 10; i++) {
        long long t0 = 1000 + i * 250;
        assert(machine_key(&m, &c, KEY_UP, 1, t0, &code) == ACT_NONE);
        assert(machine_key(&m, &c, KEY_UP, 0, t0 + 100, &code) == ACT_CLICK);
        assert(machine_key(&m, &c, KEY_DOWN, 1, t0 + 110, &code) == ACT_NONE);
        assert(machine_key(&m, &c, KEY_DOWN, 0, t0 + 200, &code) == ACT_CLICK);
    }
    c.repeat_gap_ms = 0;

    puts("all tests passed");
    return 0;
}
