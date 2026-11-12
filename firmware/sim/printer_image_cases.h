#pragma once

static void image_start(const char *fields) {
    char raw[600];
    snprintf(raw, sizeof raw, "{\"t\":\"printer_image\",\"rid\":\"%s\",%s}", RID, fields);
    receive(raw);
}
static void image_print_round_trip(void) {
    fresh(); image_credits = 0;
    image_start("\"action\":\"start\",\"head_width\":576,\"width\":300,\"height\":20");
    app_tick(); assert(callback && printer_work_busy());
    image_host = true; finish_work(); image_host = false; app_tick();
    assert(image_credits == 2 && attempts == 3 && !lease && !radio_owned);
    cJSON *j = reply(2, "sent", NULL, "submitted");
    cJSON *layout = cJSON_GetObjectItem(j,"layout"); assert(layout);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(layout,"format")),"image"));
    assert(cJSON_GetObjectItem(layout,"width_dots")->valueint == 300);
    assert(cJSON_GetObjectItem(layout,"height_dots")->valueint == 20);
    assert(rows == 109 && packets == 112 && blank_tail >= 89 && finishes == 1);
    cJSON_Delete(j);
}
static void image_invalid_and_cancel(void) {
    fresh();
    image_start("\"action\":\"start\",\"head_width\":576,\"width\":300,\"height\":20,\"height\":20");
    app_tick(); cJSON_Delete(reply(0,"error","invalid_request","not_sent"));
    assert(!opens && !packets);
    fresh();
    image_start("\"action\":\"start\",\"head_width\":576,\"width\":300,\"height\":20");
    app_tick(); assert(callback && printer_work_busy());
    image_start("\"action\":\"abort\"");
    finish_work(); app_tick(); cJSON_Delete(reply(0,"error","printer_disconnected","not_sent"));
    assert(!opens && !packets && !printer_work_busy());
    fresh();
    image_start("\"action\":\"start\",\"head_width\":576,\"width\":300,\"height\":1329");
    finish_request(); cJSON_Delete(reply(0,"error","content_does_not_fit","not_sent"));
    assert(opens == 1 && closes == 1 && !packets && !printer_work_busy());
    fresh();
    image_start("\"action\":\"start\",\"head_width\":576,\"width\":300,\"height\":20");
    app_tick(); assert(callback && printer_work_busy());
    image_host = true; send_ok = false; finish_work(); image_host = false; send_ok = true; app_tick();
    cJSON_Delete(reply(1,"error","printer_disconnected","not_sent"));
    assert(opens == 1 && closes == 1 && !packets && !printer_work_busy());
}
