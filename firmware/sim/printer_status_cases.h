#pragma once
static void status_cache_unchecked(void) {
    fresh();printer_status_t s;printer_status_snapshot(&s);
    assert(s.state==PRINTER_STATUS_UNCHECKED && s.stale && !s.busy);
    assert(s.battery_pct==-1 && s.charging==-1 && !s.name[0]);
}
static void status_cache_verified_and_failed(void) {
    fresh();battery_pct=37;request(RID,"\"action\":\"status\"");app_tick();
    printer_status_t s;printer_status_snapshot(&s);assert(s.busy && s.battery_pct==-1);
    finish_work();app_tick();printer_status_snapshot(&s);
    assert(status_at_close.battery_pct==-1 && s.state==PRINTER_STATUS_VERIFIED && !s.stale);
    assert(s.battery_pct==37 && s.charging==-1 && !strcmp(s.name,"PeriPage+test"));
    int64_t verified=s.last_verified_ms;clock_ms++;
    open_result=PERIPAGE_NOT_FOUND;request(RID,"\"action\":\"status\"");finish_request();printer_status_snapshot(&s);
    assert(s.state==PRINTER_STATUS_UNAVAILABLE && s.stale && s.battery_pct==37);
    assert(s.last_verified_ms==verified && s.last_checked_ms>verified && s.charging==-1);
    assert(!strcmp(s.error,"printer_not_found"));
}
static void status_visible_polling(void) {
    fresh();app_state.automatic_allowed=true;printer_status_set_visible(true);
    app_tick();finish_work();app_tick();assert(opens==1 && !attempts);
    clock_ms+=59999;app_tick();assert(!callback);clock_ms++;app_tick();assert(callback);
    open_result=PERIPAGE_NOT_FOUND;finish_work();app_tick();assert(opens==2 && !packets);
    clock_ms+=29999;app_tick();assert(!callback);clock_ms++;app_tick();assert(callback);
    finish_work();app_tick();app_state.automatic_allowed=false;clock_ms+=300000;app_tick();
    assert(!callback && !printer_status_refresh());
    app_state.automatic_allowed=true;app_state.online=false;app_tick();
    assert(!callback && !printer_status_refresh());
    printer_status_set_visible(false);
}
static void status_print_final_reading(void) {
    fresh();battery_pct=44;finish_battery_pct=38;
    request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"fresh final battery\"");finish_request();
    printer_status_t s;printer_status_snapshot(&s);
    assert(s.state==PRINTER_STATUS_VERIFIED && s.battery_pct==38 && s.charging==-1);
    cJSON *j=reply(0,"sent",NULL,"submitted");cJSON *p=cJSON_GetObjectItem(j,"printer");
    assert(cJSON_GetObjectItem(p,"battery_pct")->valueint==38);cJSON_Delete(j);
}
static void status_explicit_refresh_bound(void) {
    fresh();app_state.automatic_allowed=true;app_tick();finish_work();app_tick();
    printer_status_t s;printer_status_snapshot(&s);assert(!s.can_refresh && !printer_status_refresh());
    clock_ms+=60000;app_tick();assert(!callback);printer_status_snapshot(&s);assert(s.can_refresh);
    assert(printer_status_refresh() && !printer_status_refresh());app_tick();assert(callback);
    printer_status_snapshot(&s);assert(s.busy && !s.can_refresh);finish_work();app_tick();
    clock_ms+=60000;app_tick();assert(printer_status_refresh());
    app_state.online=false;app_tick();assert(!callback);
    app_state.online=true;app_tick();assert(!callback); // No deferred query after reconnect.
}
static void status_refresh_after_fault(void) {
    fresh();app_state.automatic_allowed=true;printer_status_set_visible(true);
    printer_guard_begin();printer_guard_boot(false);app_tick();assert(!callback);
    printer_status_t s;printer_status_snapshot(&s);assert(s.can_refresh && printer_status_refresh());
    app_tick();assert(callback && !printer_guard_background_allowed());finish_work();app_tick();
    printer_status_snapshot(&s);assert(s.state==PRINTER_STATUS_VERIFIED && !s.stale);
    assert(!printer_guard_background_allowed());clock_ms+=60000;app_tick();assert(!callback);
    assert(printer_status_refresh());app_tick();assert(printer_protocol_before_voice());
    assert(cancels==1 && !printer_work_busy());
    assert(!printer_guard_background_allowed());printer_guard_reset();printer_status_set_visible(false);
}
