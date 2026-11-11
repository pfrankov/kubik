#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* ACTUAL_PIPE_ROWS */
#define PANEL_W 480
#define LCD_STRIPE_MAX_ROWS 24
#define portMAX_DELAY 99
#define pdFALSE 0
#define IRAM_ATTR
#define LCD_TRANS_PIXELS ((void *)1)
#define LCD_TRANS_COMMAND ((void *)2)
#define SPI_TRANS_MODE_QIO 1
#define SPI_TRANS_VARIABLE_CMD 2
#define SPI_TRANS_VARIABLE_ADDR 4
#define SPI_TRANS_CS_KEEP_ACTIVE 8
#define ESP_ERROR_CHECK(v) assert((v)==0)
#define portYIELD_FROM_ISR() ((void)0)
typedef int BaseType_t;
typedef struct {int cmd,addr,length;const uint16_t *tx_buffer;int flags;void *user;} spi_transaction_t;
typedef struct {spi_transaction_t base;int command_bits,address_bits;} spi_transaction_ext_t;
static bool s_lcd_stream=true,s_lcd_first=true;
#if !CONFIG_IDF_TARGET_ESP32S3
static bool s_lcd_reserved;
#endif
static int s_lcd=1,s_lcd_slots=2,s_lcd_free=3;
static int credits=LCD_DMA_CREDITS,takes,queued,completed,descriptor_returns;
static spi_transaction_ext_t transactions[16];
static void lcd_trans_done(spi_transaction_t *t);
static void xSemaphoreGiveFromISR(int sem,BaseType_t *woken) {
    if(sem==s_lcd_slots){assert(credits<LCD_DMA_CREDITS);credits++;}
    else {assert(sem==s_lcd_free);descriptor_returns++;}
    *woken=1;
}
static void xSemaphoreTake(int sem,int wait) {
    assert(sem==s_lcd_slots && wait==portMAX_DELAY);takes++;
    if(!credits){assert(completed<queued);lcd_trans_done(&transactions[completed++].base);}
    assert(credits>0);credits--;
}
static spi_transaction_ext_t *lcd_trans_next(void) {assert(queued<16);return &transactions[queued];}
static int spi_device_queue_trans(int lcd,spi_transaction_t *t,int wait) {
    assert(lcd==s_lcd && wait==portMAX_DELAY && t==&transactions[queued].base);
    assert(t->user==LCD_TRANS_PIXELS && t->length==32 && t->tx_buffer);queued++;return 0;
}
/* ACTUAL_FUNCTIONS */
static void invalid_case(int argc,char **argv,uint16_t *pixels) {
#if !CONFIG_IDF_TARGET_ESP32S3
    if(argc>1) {
        if(argv[1][0]=='w')board_display_write(pixels,2,true);
        display_reserve_buffer();
        if(argv[1][0]=='r')display_reserve_buffer();
        else board_display_end();
        assert(false);
    }
#else
    (void)argc;(void)argv;(void)pixels;
#endif
}
static void queue_pixels(uint16_t *pixels) {
    for(int i=0;i<5;i++) {
        int before=takes;
#if !CONFIG_IDF_TARGET_ESP32S3
        display_reserve_buffer();assert(s_lcd_reserved && takes==before+1);before=takes;
#endif
        board_display_write(pixels,2,i==4);
#if CONFIG_IDF_TARGET_ESP32S3
        assert(takes==before+1);
#else
        assert(takes==before && !s_lcd_reserved);
#endif
    }
}
int main(int argc,char **argv) {
    uint16_t pixels[2]={0};invalid_case(argc,argv,pixels);queue_pixels(pixels);
    assert(queued==5 && completed==5-LCD_DMA_CREDITS && credits==0);
    board_display_end();assert(!s_lcd_stream && credits==0); // tails still own credits
    spi_transaction_t command={.user=LCD_TRANS_COMMAND};lcd_trans_done(&command);assert(credits==0);
    while(completed<queued)lcd_trans_done(&transactions[completed++].base);
    assert(credits==LCD_DMA_CREDITS && descriptor_returns==6);
    puts("actual board DMA: C6 reservation is consumed once; S3 acquires on submit; only pixel completion releases credits");
}
