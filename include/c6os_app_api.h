#ifndef C6OS_APP_API_H
#define C6OS_APP_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard C runtime imports resolved by the C6-MicroLinux RV32IMAC ELF loader */
int printf(const char* fmt, ...);
int puts(const char* s);
int putchar(int c);
int snprintf(char* buf, size_t cap, const char* fmt, ...);
void* malloc(size_t sz);
void free(void* ptr);

/* C6-MicroLinux Hardware & Display SDK imports */
int c6_lcd_text(const char* headline, const char* subtitle);
int c6_lcd_mode(const char* mode_name);
int c6_lcd_theme(const char* theme_name);
int c6_backlight(int pct);
int c6_led_rgb(int r, int g, int b);
int c6_gpio_mode(int pin, int mode);   /* 1=out, 2=in, 3=pullup, 4=pulldown */
int c6_gpio_write(int pin, int val);
int c6_gpio_read(int pin);
int c6_pwm_write(int pin, int duty);
int c6_adc_read_mv(int pin);
int c6_i2c_scan(uint8_t* out_addrs, int max_addrs);
uint32_t c6_uptime_ms(void);
uint32_t c6_free_ram(void);
void c6_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* C6OS_APP_API_H */
