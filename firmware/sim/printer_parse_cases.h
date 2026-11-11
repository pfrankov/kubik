#pragma once
static void parser_fields(void) {
    fresh();
    const char *fields[] = {"\"action\":\"other\"", "\"action\":2", "\"action\":\"print\",\"emoji\":[]",
        "\"action\":\"status\",\"text\":\"\"", "\"action\":\"status\",\"width_mm\":20",
        "\"action\":\"status\",\"copies\":2", "\"action\":\"status\",\"action\":\"print\",\"emoji\":[]",
        "\"action\":\"print\",\"emoji\":[],\"text\":\"safe\",\"format\":\"image\"",
        "\"action\":\"print\",\"text\":\"safe\",\"format\":\"qr\",\"font_mm\":2",
        "\"action\":\"print\",\"emoji\":[],\"text\":\"safe\",\"width_mm\":0",
        "\"action\":\"print\",\"emoji\":[],\"text\":\"safe\",\"height_mm\":151",
        "\"action\":\"print\",\"emoji\":[],\"text\":\"safe\",\"font_mm\":1.4"};
    for (unsigned i=0;i<sizeof fields/sizeof fields[0];i++) invalid(fields[i],strstr(fields[i],"\"action\":\"print\"") == fields[i]);
    unsigned before=attempts;receive("{\"t\":\"printer\",\"action\":\"status\"}");app_tick();assert(attempts==before);
    request("ABCDEF0123456789abcdef0123456789ab","\"action\":\"status\"");app_tick();assert(attempts==before);
    receive("{\"t\":\"welcome\"}");assert(dispatched==1);
}
static void text_invalid(const char *value) {
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"t","printer");cJSON_AddStringToObject(j,"rid",RID);
    cJSON_AddStringToObject(j,"action","print");cJSON_AddStringToObject(j,"text",value);
    cJSON_AddArrayToObject(j,"emoji");
    char *raw=cJSON_PrintUnformatted(j);cJSON_Delete(j);unsigned before=attempts;
    receive(raw);free(raw);app_tick();assert(attempts==before+1);
    cJSON_Delete(reply(before,"error","invalid_request","not_sent"));assert(!opens);
}
static void parser_text(void) {
    fresh();const char *values[]={""," \n\t","\xc2\xa0\xe3\x80\x80","\xef\xbb\xbf","x\r","x\x1b@","x\x7f",
        "x\xc2\x80","x\xc0\xaf","x\xed\xa0\x80","x\xf4\x90\x80\x80","x\x80","x\xe2\x82"};
    for(unsigned i=0;i<sizeof values/sizeof values[0];i++)text_invalid(values[i]);
    char text[1026];memset(text,'x',1025);text[1025]=0;text_invalid(text);
    char qr[400];memset(qr,'x',257);qr[257]=0;
    char fields[600];snprintf(fields,sizeof fields,"\"action\":\"print\",\"format\":\"qr\",\"text\":\"%s\"",qr);invalid(fields,true);
}
static void parser_raw(void) {
    fresh();
    invalid("\"action\":\"print\",\"emoji\":[],\"text\":\"safe\\u0000tail\"",true);
    invalid("\"action\":\"print\",\"emoji\":[],\"text\":\"raw\nnewline\"",true);
    char raw[220];snprintf(raw,sizeof raw,"{\"t\":\"printer\",\"rid\":\"%s\",\"action\":\"status\"}{}",RID);
    receive(raw);app_tick();cJSON_Delete(reply(2,"error","invalid_request",NULL));
    snprintf(raw,sizeof raw,"{\"t\":\"printer\",\"rid\":\"%s\",\"action\":\"status\"}",RID);
    size_t n=strlen(raw);raw[n]=0;memcpy(raw+n+1,"trailing",9);
    receive_bytes(raw,n+9);app_tick();cJSON_Delete(reply(3,"error","invalid_request",NULL));
}
