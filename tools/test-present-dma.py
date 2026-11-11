#!/usr/bin/env python3
"""Exercise real C6 window chunking and board credits with delayed DMA reads."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
board = (ROOT / 'firmware/main/board.h').read_text()
present = (ROOT / 'firmware/main/present.h').read_text()
pipe = re.search(r'#if CONFIG_IDF_TARGET_ESP32S3\n#define LCD_PIPE 3\n#define LCD_DMA_CREDITS \(LCD_PIPE - 1\)\n#else\n#define LCD_PIPE 2\n#define LCD_DMA_CREDITS LCD_PIPE\n#endif', board)
rows = re.search(r'#if CONFIG_IDF_TARGET_ESP32S3\n#define OUTPUT_ROWS 8\n#else\n#define OUTPUT_ROWS 6[^\n]*\n#endif', present)
assert pipe and rows
macros = pipe[0] + '\n' + rows[0]
driver = (ROOT / 'firmware/main/board.c').read_text()
assert 'xSemaphoreCreateCounting(LCD_DMA_CREDITS, LCD_DMA_CREDITS)' in driver
assert 'if (t->user == LCD_TRANS_PIXELS) xSemaphoreGiveFromISR(s_lcd_slots, &woken)' in driver


def function(source, declaration):
    start = source.index(declaration)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


clear = function(driver, 'void display_panel_init(')
assert clear.index('display_reserve_buffer();') < clear.index('board_display_write(black,')
source = (ROOT / 'firmware/main/present.c').read_text()
send = function(source, 'static void send_window(')
c6_send = send[send.index('#else'):send.rindex('#endif')]
assert c6_send.index('display_reserve_buffer();') < c6_send.index('f->bufs[f->next]') < c6_send.index('compose_window_rows(')
fixture = (ROOT / 'firmware/sim/present_dma_test.c').read_text().replace('/* ACTUAL_PIPE_ROWS */', macros)
board_fixture = (ROOT / 'firmware/sim/board_dma_credit_test.c').read_text().replace('/* ACTUAL_PIPE_ROWS */', macros)
functions = function(driver, 'static void IRAM_ATTR lcd_trans_done(')
functions += '#if !CONFIG_IDF_TARGET_ESP32S3\n' + function(driver, 'void display_reserve_buffer(') + '#endif\n'
functions += function(driver, 'void board_display_write(') + function(driver, 'void board_display_end(')
board_fixture = board_fixture.replace('/* ACTUAL_FUNCTIONS */', functions)
flags = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
with tempfile.TemporaryDirectory(prefix='kubik-present-dma-') as temporary:
    path = Path(temporary) / 'dma.c'
    binary = Path(temporary) / 'dma'
    path.write_text(fixture.replace('/* ACTUAL_SEND_WINDOW */', send))
    subprocess.run([*flags, '-DCONFIG_IDF_TARGET_ESP32S3=0', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    # Keeping the old wait-inside-write placement with all credits overwrites A
    # before it can wait. The delayed-source assertion must catch the first buffer reuse.
    legacy = re.sub(r'^\s*display_reserve_buffer\(\);[^\n]*\n', '', send, flags=re.MULTILINE)
    path.write_text(fixture.replace('/* ACTUAL_SEND_WINDOW */', legacy))
    subprocess.run([*flags, '-DCONFIG_IDF_TARGET_ESP32S3=0', '-DDMA_OLD_WAIT_AFTER_COMPOSE=1', str(path), '-o', str(binary)], check=True)
    failure = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert failure.returncode and 'out!=pending[i].data' in failure.stderr
    print('PASS negative contrast: waiting on submit with all credits touches a still-owned source')
    for s3 in (0, 1):
        path.write_text(board_fixture)
        subprocess.run([*flags, f'-DCONFIG_IDF_TARGET_ESP32S3={s3}', str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
        if not s3:
            for invalid in ('write-without-reserve', 'reserve-twice', 'end-while-reserved'):
                failure = subprocess.run([str(binary), invalid], capture_output=True, timeout=10)
                assert failure.returncode, invalid
    path.write_text(macros + '\n_Static_assert(LCD_PIPE==3 && OUTPUT_ROWS==8 && LCD_DMA_CREDITS==2,"S3 keeps three native slots and submit credits");int main(void){return 0;}')
    subprocess.run(['cc', '-std=c11', '-DCONFIG_IDF_TARGET_ESP32S3=1', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
