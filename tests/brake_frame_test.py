import pathlib, subprocess, tempfile
source = pathlib.Path("components/bsp/src/motor_driver.c").read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]
harness = """#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
typedef int esp_err_t;
#define MOTOR_FRAME_LEN 10
#define MOTOR_CMD_DRIVE 0x64
static uint8_t captured[10];
static esp_err_t motor_uart_write(uint8_t *frame);
""" + function("uint8_t crc8_maxim(") + """
static esp_err_t motor_uart_write(uint8_t *frame){frame[9]=crc8_maxim(frame,9);memcpy(captured,frame,10);return 0;}
""" + function("esp_err_t motor_brake(") + """
int main(void){const uint8_t expected[10]={1,0x64,0,0,0,0,0,0xff,0,0xd1};assert(motor_brake(1)==0);assert(memcmp(captured,expected,10)==0);}
"""
with tempfile.TemporaryDirectory() as directory:
    test = pathlib.Path(directory)/"brake.c"
    test.write_text(harness)
    binary = pathlib.Path(directory)/"brake"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror",str(test),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print("BRAKE_FRAME_PASS")
