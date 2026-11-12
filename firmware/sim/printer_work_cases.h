#pragma once
static void status_and_print(void) {
    fresh();request(RID,"\"action\":\"status\"");assert(!opens);finish_request();
    cJSON *j=reply(0,"ready",NULL,NULL);cJSON *p=cJSON_GetObjectItem(j,"printer");assert(p);
    assert(cJSON_GetObjectItem(p,"width_dots")->valueint==576);assert(cJSON_GetObjectItem(p,"dpi")->valueint==304);
    assert(cJSON_GetObjectItem(p,"feed_dpi")->valueint==304);assert(cJSON_IsNull(cJSON_GetObjectItem(p,"charging")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(p,"media_height_mm")));cJSON_Delete(j);assert(!packets);
    printf("PIPELINE_WIRE %s\n",sent[0]);assert(reaps==1 && releases==1);
    request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"Привет, Кубик!\\nMilk\\t2\",\"width_mm\":30,\"height_mm\":20,\"font_mm\":3");
    finish_request();j=reply(1,"sent",NULL,"submitted");p=cJSON_GetObjectItem(j,"layout");assert(p);
    unsigned height=(unsigned)cJSON_GetObjectItem(p,"height_dots")->valueint;assert(height==239);
    assert(rows==height+120 && last_ink_row<=height && blank_tail>=120);assert(packets==rows+6);assert(printed_ink);cJSON_Delete(j);
    assert(finishes==1 && budget_calls==1 && planned_packets==packets);
    printf("PIPELINE_WIRE %s\n",sent[1]);assert(!printer_work_busy());
}
static void job_errors(void) {
    fresh();open_result=PERIPAGE_NOT_FOUND;request(RID,"\"action\":\"status\"");finish_request();
    cJSON_Delete(reply(0,"error","printer_not_found",NULL));assert(!packets && closes==1);
    fresh();request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\",\"width_mm\":1");finish_request();
    cJSON_Delete(reply(0,"error","content_does_not_fit","not_sent"));assert(!packets);
    fresh();request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"\xf0\x9f\x98\x80\"");finish_request();
    cJSON_Delete(reply(0,"error","unsupported_character","not_sent"));assert(!packets);
    fresh();fail_packet=4;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");finish_request();
    cJSON_Delete(reply(0,"error","printer_disconnected","unknown"));assert(packets==4 && closes==1);
    fresh();close_ok=false;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");finish_request();
    cJSON_Delete(reply(0,"error","printer_failed","unknown"));assert(!radio_owned);
}
static void queued_flood(void) {
    fresh();request(RID,"\"action\":\"status\"");
    for(unsigned i=0;i<1000;i++){char rid[33];snprintf(rid,sizeof rid,"%032x",i);request(rid,"\"action\":\"status\"");}
    for(unsigned i=0;i<PRINTER_REPLY_SLOTS;i++) { app_tick(); }
    assert(attempts==PRINTER_REPLY_SLOTS);
    for(unsigned i=0;i<10;i++) { app_tick(); }
    assert(attempts==PRINTER_REPLY_SLOTS);
    finish_work();app_tick();assert(attempts==PRINTER_REPLY_SLOTS+1);assert(opens==1);
}
static void no_replay_and_sessions(void) {
    queued_flood();fresh();request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");app_tick();
    fail_alloc=true;for(unsigned i=0;i<20;i++)request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");
    request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":false,\"unknown\":0");
    assert(fail_alloc);fail_alloc=false;finish_work();app_tick();assert(attempts==1 && opens==1);
    fresh();request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");session++;app_tick();assert(!opens && !attempts);
    fresh();change_session_packet=4;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");finish_request();
    assert(!attempts && packets==4);assert(!printer_work_busy());
    fresh();send_ok=false;request(RID,"\"action\":\"status\"");finish_request();
    for(unsigned i=0;i<10;i++) { app_tick(); }
    assert(attempts==1 && opens==1);
}
static void automatic_discovery(void) {
    fresh();app_state.automatic_allowed=true;app_tick();assert(callback && !opens);finish_work();app_tick();
    assert(opens==1 && !attempts && !packets);for(unsigned i=0;i<20;i++)app_tick();assert(!callback);
    assert(printer_protocol_discovered());session++;app_tick();assert(!callback && !printer_protocol_discovered());
    clock_ms+=29999;app_tick();assert(!callback);clock_ms++;app_tick();assert(callback);finish_work();app_tick();
    fresh();open_result=PERIPAGE_NOT_FOUND;app_state.automatic_allowed=true;app_tick();finish_work();app_tick();
    clock_ms+=29999;app_tick();assert(!callback);clock_ms++;app_tick();assert(callback);finish_work();app_tick();
    clock_ms+=59999;app_tick();assert(!callback);clock_ms++;app_tick();assert(callback);finish_work();app_tick();
    clock_ms+=299999;app_tick();assert(!callback);clock_ms++;app_tick();assert(callback);finish_work();app_tick();
    assert(opens==4 && !packets && !attempts);
}
static void discovery_voice_admission(void) {
    fresh();app_state.automatic_allowed=true;app_tick();assert(printer_protocol_before_voice());
    assert(cancels==1 && !printer_work_busy() && !lease);assert(!opens && !attempts);
    fresh();app_state.automatic_allowed=true;app_tick();defer_work=true;int64_t start=clock_ms;
    assert(!printer_protocol_before_voice());assert(clock_ms-start==2000 && lease && callback);
    assert(printer_work_busy() && !opens && !attempts);defer_work=false;finish_work();app_tick();
    assert(printer_protocol_before_voice() && !lease && !printer_work_busy());
}
static void foreground_voice_unchanged(void) {
    fresh();request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");app_tick();
    assert(printer_protocol_before_voice() && !cancels && callback && lease);finish_work();app_tick();
}
static void diagnostic_snapshot(void) {
    fresh();printer_diagnostics_t d;printer_work_diagnostics(&d);
    assert(d.state.session==session && d.observed_session==session && d.guard_allowed);
    assert(!d.busy && !d.discovered && !d.attempts);
    app_state.automatic_allowed=true;open_result=PERIPAGE_NOT_FOUND;app_tick();finish_work();app_tick();
    printer_work_diagnostics(&d);assert(d.attempts==1 && d.due_in_ms==30000 && !d.busy);
    printer_guard_begin();printer_guard_boot(false);app_tick();
    char raw[1024];status_json_t info;status_json_begin(&info,raw,sizeof raw);
    printer_protocol_info(&info);assert(status_json_end(&info));cJSON *j=cJSON_Parse(raw);assert(j);
    assert(cJSON_GetObjectItem(j,"printer_session")->valueint==(int)session);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(j,"printer_guard_allowed")));
    assert(cJSON_GetObjectItem(j,"printer_due_ms")->valueint==30000);
    assert(cJSON_GetObjectItem(j,"reset_reason")->valueint==1);cJSON_Delete(j);printer_guard_reset();
}
static void resource_and_deadline(void) {
    fresh();fail_alloc=true;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");app_tick();
    cJSON_Delete(reply(0,"error","printer_failed","not_sent"));assert(!opens);
    fresh();launch_ok=false;request(RID,"\"action\":\"status\"");app_tick();
    cJSON_Delete(reply(0,"error","printer_failed",NULL));assert(!printer_work_busy());
    fresh();app_state.can_start=false;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\"");app_tick();
    assert(printer_work_busy() && !opens);assert(!printer_protocol_reset_ready());
    clock_ms+=78000;app_tick();cJSON_Delete(reply(0,"error","printer_busy","not_sent"));assert(!printer_work_busy());
    fresh();packet_ms=100;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"safe\",\"height_mm\":150");
    int64_t start=clock_ms;finish_request();cJSON_Delete(reply(0,"error","printer_disconnected","unknown"));
    assert(clock_ms-start<=78000);assert(packets<1000 && !radio_owned);
}
static void stream_budget_and_finish(void) {
    fresh();packet_ms=20;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"budget fits\",\"height_mm\":150");
    clock_ms+=33220;finish_request();cJSON_Delete(reply(0,"sent",NULL,"submitted"));
    assert(rows==1915 && packets==1939 && planned_packets==1939 && finishes==1);
    fresh();packet_ms=20;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"one ms too late\",\"height_mm\":150");
    clock_ms+=33221;finish_request();cJSON_Delete(reply(0,"error","printer_busy","not_sent"));
    assert(planned_packets==1939 && !packets && !finishes && closes==1);
    fresh();finish_ok=false;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"final query fails\"");
    finish_request();cJSON_Delete(reply(0,"error","printer_disconnected","unknown"));
    assert(finishes==1 && packets==planned_packets && closes==1);
}
static void assert_tear_result(unsigned trailing) {
    cJSON *j=reply(0,"sent",NULL,"submitted");
    cJSON *layout=cJSON_GetObjectItem(j,"layout");assert(layout);
    unsigned content=(unsigned)cJSON_GetObjectItem(layout,"height_dots")->valueint;
    assert(printed_ink && rows==content+trailing && last_ink_row<=content);
    assert(blank_tail>=trailing && !raster_remaining && declared_rows==rows);
    assert(raster_headers==(rows+254)/255 && packets==rows+3*raster_headers);
    assert(planned_packets==packets && finishes==1);cJSON_Delete(j);
}
static void tear_off_tail(void) {
    static const unsigned axes[]={152,203,304};
    static const unsigned trailing[]={60,80,120};
    for(unsigned i=0;i<3;i++)for(unsigned qr=0;qr<2;qr++) {
        fresh();profile_feed_dpi=axes[i];
        request(RID,qr ? "\"action\":\"print\",\"format\":\"qr\",\"text\":\"https://kubik.local\",\"width_mm\":20,\"height_mm\":20" :
            "\"action\":\"print\",\"emoji\":[],\"text\":\"A\",\"font_mm\":3");
        finish_request();
        if(qr && axes[i]==203) {
            cJSON_Delete(reply(0,"error","content_does_not_fit","not_sent"));
            assert(!packets && !finishes);continue;
        }
        assert_tear_result(trailing[i]);
    }
}
static void tail_failure_no_replay(void) {
    fresh();fail_packet=1939;request(RID,"\"action\":\"print\",\"emoji\":[],\"text\":\"tail failure\",\"height_mm\":150");
    finish_request();cJSON_Delete(reply(0,"error","printer_disconnected","unknown"));
    assert(packets==1939 && blank_tail>=120 && !finishes && closes==1);
    for(unsigned i=0;i<10;i++) { app_tick(); }
    assert(packets==1939 && attempts==1 && opens==1 && !printer_work_busy());
}
static void crash_guard(void) {
    fresh();printer_guard_begin();printer_guard_boot(false);assert(!printer_guard_background_allowed());
    app_state.automatic_allowed=true;app_tick();assert(!callback);
    request(RID,"\"action\":\"status\"");finish_request();cJSON_Delete(reply(0,"ready",NULL,NULL));
    assert(!printer_guard_background_allowed());printer_guard_boot(false);assert(printer_guard_background_allowed());
    fresh();printer_job_t *job=calloc(1,sizeof *job);assert(job);job->session=session;job->deadline_ms=clock_ms+8000;
    close_stuck=true;lease=true;bool found=false;
    if(setjmp(restarted)==0){char *j=printer_job_run(job,&found);free(j);assert(false);}
    assert(radio_owned);printer_guard_boot(false);assert(!printer_guard_background_allowed());
    radio_owned=false;lease=false;close_stuck=false;free(job);printer_guard_boot(true);assert(printer_guard_background_allowed());
}
