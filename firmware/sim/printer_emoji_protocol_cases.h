#pragma once
static void emoji_bitmap(char out[173]) {
    memset(out,'/',170);out[170]='8';out[171]='=';out[172]=0;
}
static cJSON *emoji_glyph(const char *sequence, const char *bitmap) {
    cJSON *glyph=cJSON_CreateObject();assert(glyph);
    cJSON_AddStringToObject(glyph,"sequence",sequence);cJSON_AddStringToObject(glyph,"bitmap",bitmap);
    return glyph;
}
static cJSON *emoji_array(const char *sequence, const char *bitmap) {
    cJSON *array=cJSON_CreateArray();assert(array);cJSON_AddItemToArray(array,emoji_glyph(sequence,bitmap));return array;
}
static void emoji_request(cJSON *array, const char *text) {
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"t","printer");cJSON_AddStringToObject(j,"rid",RID);
    cJSON_AddStringToObject(j,"action","print");cJSON_AddStringToObject(j,"text",text);
    cJSON_AddItemToObject(j,"emoji",array);char *raw=cJSON_PrintUnformatted(j);cJSON_Delete(j);assert(raw);
    receive(raw);memset(raw,0xa5,strlen(raw));free(raw);
}
static void emoji_rejected(cJSON *array, const char *text) {
    fresh();emoji_request(array,text);app_tick();cJSON_Delete(reply(0,"error","invalid_request","not_sent"));assert(!opens);
}
static void emoji_required_and_foreign(void) {
    fresh();request(RID,"\"action\":\"print\",\"text\":\"missing emoji array\"");app_tick();
    cJSON_Delete(reply(0,"error","invalid_request","not_sent"));
    fresh();invalid("\"action\":\"status\",\"emoji\":[]",false);
    invalid("\"action\":\"print\",\"format\":\"qr\",\"text\":\"safe\",\"emoji\":[]",true);
    emoji_rejected(cJSON_CreateObject(),"bad array");emoji_rejected(cJSON_CreateNull(),"bad array");
    char bitmap[173];emoji_bitmap(bitmap);emoji_rejected(emoji_array("😀",bitmap),"unused glyph");
}
static void emoji_bad_shape_and_sequences(void) {
    char bitmap[173];emoji_bitmap(bitmap);cJSON *array=emoji_array("😀",bitmap);
    cJSON_AddStringToObject(array->child,"unknown","x");emoji_rejected(array,"😀");
    array=emoji_array("😀",bitmap);cJSON_AddStringToObject(array->child,"sequence","😀");emoji_rejected(array,"😀");
    array=emoji_array("😀",bitmap);cJSON_AddItemToArray(array,emoji_glyph("😀",bitmap));emoji_rejected(array,"😀");
    const char *sequences[]={"😀","😁","😂","😃","😄","😅","😆","😉","😊"};
    array=cJSON_CreateArray();for(unsigned i=0;i<9;i++)cJSON_AddItemToArray(array,emoji_glyph(sequences[i],bitmap));
    emoji_rejected(array,"😀😁😂😃😄😅😆😉😊");emoji_rejected(emoji_array("😀\n",bitmap),"😀\n");
    char long_sequence[66];memset(long_sequence,'x',65);long_sequence[65]=0;
    emoji_rejected(emoji_array(long_sequence,bitmap),long_sequence);
    emoji_rejected(emoji_array("\xc0\xaf",bitmap),"😀");
}
static void emoji_bad_base64(void) {
    char bitmap[173];emoji_bitmap(bitmap);bitmap[170]='9';emoji_rejected(emoji_array("😀",bitmap),"😀");
    emoji_bitmap(bitmap);bitmap[0]='_';emoji_rejected(emoji_array("😀",bitmap),"😀");
    emoji_bitmap(bitmap);bitmap[171]=0;emoji_rejected(emoji_array("😀",bitmap),"😀");
    emoji_bitmap(bitmap);bitmap[171]='A';emoji_rejected(emoji_array("😀",bitmap),"😀");
    emoji_bitmap(bitmap);bitmap[20]='=';emoji_rejected(emoji_array("😀",bitmap),"😀");
}
static void emoji_owned_until_print(void) {
    fresh();char bitmap[173];emoji_bitmap(bitmap);emoji_request(emoji_array("😀",bitmap),"😀");
    assert(last_job_allocation==sizeof(printer_job_t)+strlen("😀")+1+sizeof(printer_emoji_glyph_t));
    assert(printer_work_busy() && !opens);finish_request();cJSON_Delete(reply(0,"sent",NULL,"submitted"));
    assert(printed_ink && finishes==1 && packets==planned_packets);
}
static void emoji_owned_eight(void) {
    fresh();char bitmap[173];emoji_bitmap(bitmap);cJSON *array=cJSON_CreateArray();
    const char *sequences[]={"😀","😁","😂","😃","😄","😅","😆","😉"};
    const char *text="😀😁😂😃😄😅😆😉";
    for(unsigned i=0;i<8;i++)cJSON_AddItemToArray(array,emoji_glyph(sequences[i],bitmap));
    emoji_request(array,text);assert(last_job_allocation==sizeof(printer_job_t)+strlen(text)+1+8*sizeof(printer_emoji_glyph_t));
    finish_request();cJSON_Delete(reply(0,"sent",NULL,"submitted"));assert(printed_ink && finishes==1);
}
