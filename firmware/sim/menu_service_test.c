#include <assert.h>
#include "../main/menu_service.h"
int main(void) {
    menu_service_t s = {0};
    for (int i = 0; i < 4; i++) assert(!menu_service_tap(&s, 430, 30, i * 500));
    assert(!s.unlocked);
    assert(menu_service_tap(&s, 430, 30, 2000));
    assert(s.unlocked);
    assert(!menu_service_tap(&s, 430, 30, 2500));
    s = (menu_service_t){0};
    for (int i = 0; i < 10; i++) assert(!menu_service_tap(&s, 430, 30, i * 1500));
    assert(!s.unlocked);
    s = (menu_service_t){0};
    for (int i = 0; i < 4; i++) assert(!menu_service_tap(&s, 430, 30, i * 100));
    assert(!menu_service_tap(&s, 200, 30, 500));
    assert(!menu_service_tap(&s, 430, 30, 600));
    assert(!s.unlocked);
    return 0;
}
