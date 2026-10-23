/* Finds how the connector's LED is wired: cycles four candidate settings, announcing each
 * one aloud, so you can say which number made the LED light up. */
#include "rt2570.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void announce(int n, const char *what)
{
    char cmd[128];
    printf("\n>>> Test %d: %s\n", n, what);
    fflush(stdout);
    snprintf(cmd, sizeof cmd, "say 'Test %d' &", n);
    system(cmd);
}

int main(void)
{
    rt2570_t d;
    if (rt2570_open(&d)) return 1;
    if (rt2570_start_ap(&d, 6)) return 1;
    uint16_t gpio;
    rt2570_reg_read(&d, MAC_CSR19, &gpio);
    printf("Watch the LED on the Nintendo connector. Each test lasts 6 seconds.\n");
    sleep(2);

    announce(1, "LED register: link + activity + active-high polarity");
    rt2570_reg_write(&d, MAC_CSR20, 0x0007);
    sleep(6);
    announce(2, "LED register: link + activity, active-low polarity");
    rt2570_reg_write(&d, MAC_CSR20, 0x0003);
    sleep(6);
    rt2570_reg_write(&d, MAC_CSR20, 0x0000);
    announce(3, "GPIO outputs 1-7 driven high");
    rt2570_reg_write(&d, MAC_CSR19, (uint16_t)((gpio & 0xff00) | 0x00fe));
    sleep(6);
    announce(4, "GPIO outputs 1-7 driven low, LED polarity bit set");
    rt2570_reg_write(&d, MAC_CSR19, (uint16_t)(gpio & 0xff00));
    rt2570_reg_write(&d, MAC_CSR20, 0x0004);
    sleep(6);

    rt2570_reg_write(&d, MAC_CSR19, gpio);
    rt2570_reg_write(&d, MAC_CSR20, 0x0000);
    system("say 'LED test finished'");
    printf("\nDone. Which test number(s) lit the LED (or made it blink)?\n");
    rt2570_stop(&d);
    rt2570_close(&d);
    return 0;
}
