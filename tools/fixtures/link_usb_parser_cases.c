// Additional cases use the actual parser, dispatch and USB task from link_usb.c.
static unsigned parser_configs, parser_routes, parser_hellos;
static size_t parser_expected_len;
static uint8_t parser_expected_type;
static uint8_t parsed_request[LINK_JSON_MAX+1];
static unsigned parser_frees;
static void *parser_parse(const char *text,size_t len) {
    assert(text==usb_frame_allocation && len==parser_expected_len && text[len]==0);
    memcpy(parsed_request,text,len+1);return parsed_request;
}
static void parser_config(void *parsed,char *reply,size_t cap) {
    assert(parsed==parsed_request && reply==usb_frame_allocation);
    assert(cap==6144 && usb_frame_bytes==6144);
    memset(reply,'r',6000);reply[6000]=0;parser_configs++;
}
static void parser_free(void *parsed) { assert(parsed==parsed_request);memset(parsed_request,0,sizeof parsed_request);parser_frees++; }
static void parser_routed(uint8_t type,uint8_t *data,size_t len) {
    assert(type==parser_expected_type && len==parser_expected_len);
    assert(data==usb_frame_allocation && data[len]==0 && usb_frame_bytes==len+1);
    parser_routes++;
}
static void parser_hello(const char *data,size_t len) {
    assert(data==usb_frame_allocation && len==parser_expected_len && data[len]==0);
    assert(usb_frame_bytes==len+1);parser_hellos++;
}
static void parser_bytes(usb_parser_t *parser,const uint8_t *data,size_t len) {
    parser_expire(parser);
    for(size_t i=0;i<len;i++) {
        if(!parser_feed_byte(parser,data[i]))continue;
        dispatch_frame(parser);
        parser_reset(parser);
    }
}
static size_t parser_packet(uint8_t *out,uint8_t type,const uint8_t *payload,size_t len) {
    out[0]=0xa5;out[1]=0x5a;out[2]=type;out[3]=(uint8_t)len;out[4]=(uint8_t)(len>>8);
    memcpy(out+5,payload,len);out[len+5]=link_usb_crc8(0,payload,len);return len+6;
}
static void parser_error_reply(const char *expected) {
    assert(usb_size>6 && usb_bytes[2]==F_CONFIG_REPLY);
    size_t n=usb_bytes[3]|(size_t)usb_bytes[4]<<8;char text[128];
    assert(n<sizeof text && usb_size==n+6);memcpy(text,usb_bytes+5,n);text[n]=0;
    assert(strstr(text,expected) && link_usb_crc8(0,usb_bytes+5,n)==usb_bytes[n+5]);
}
static bool parser_info(void *parsed,status_json_sink_t sink,void *context,bool *ok) {
    const char request[]="{\"cmd\":\"info\"}";
    if(parsed!=parsed_request || memcmp(parsed,request,sizeof request))return false;
    *ok=true;
    for(unsigned i=0;i<6000;i++) if(!sink(context,'s')) {*ok=false;break;}
    return true;
}
static void parser_stream_info(void) {
    const uint8_t request[]="{\"cmd\":\"info\"}";
    parser_expected_len=sizeof request-1;usb_size=0;
    input_frame(F_CONFIG,request,parser_expected_len,true);run_usb_rx();
    assert(!usb_frame_allocation && !usb_chunk_live && usb_chunk_max<=416);
    assert(usb_size==6006 && (usb_bytes[3]|(unsigned)usb_bytes[4]<<8)==6000);
    for(unsigned i=0;i<6000;i++)assert(usb_bytes[i+5]=='s');
    assert(link_usb_crc8(0,usb_bytes+5,6000)==usb_bytes[6005]);
    usb_chunk_failures=1;usb_size=0;
    input_frame(F_CONFIG,request,parser_expected_len,true);run_usb_rx();
    assert(!usb_frame_allocation && !usb_chunk_live);parser_error_reply("status unavailable");
}
static void parser_limits(void) {
    uint8_t payload[LINK_USB_PAYLOAD_MAX];memset(payload,0x55,sizeof payload);
    uint8_t frame[LINK_USB_PAYLOAD_MAX+6];usb_parser_t parser={0};
    parser_expected_len=4096;
    size_t bytes=parser_packet(frame,F_CONFIG,payload,parser_expected_len);
    unsigned allocated=usb_frame_allocations;
    parser_bytes(&parser,frame,4);assert(!usb_frame_allocation && usb_frame_allocations==allocated);
    parser_bytes(&parser,frame+4,1);assert(usb_frame_bytes==4097);
    usb_size=0;parser_bytes(&parser,frame+5,bytes-5);
    assert(parser_configs==1 && !usb_frame_allocation && usb_size==6006);
    assert(usb_bytes[2]==F_CONFIG_REPLY && (usb_bytes[3]|(unsigned)usb_bytes[4]<<8)==6000);
    for(unsigned i=0;i<6000;i++)assert(usb_bytes[i+5]=='r');
    assert(link_usb_crc8(0,usb_bytes+5,6000)==usb_bytes[6005]);
    parser_expected_type=F_AUDIO;parser_expected_len=LINK_USB_PAYLOAD_MAX;
    input_frame(F_AUDIO,payload,sizeof payload,true);run_usb_rx();
    assert(parser_routes==1 && !usb_frame_allocation && usb_frame_last_bytes==4807);
    parser_expected_len=2;
    input_frame(F_HOST_HELLO,payload,2,true);run_usb_rx();
    assert(parser_hellos==1 && !usb_frame_allocation && usb_frame_last_bytes==3);
    unsigned released=usb_frame_frees;usb_size=0;
    input_frame(F_CONFIG,payload,20,false);run_usb_rx();
    assert(parser_configs==1 && usb_frame_frees==released+1 && !usb_frame_allocation && usb_size==0);
}
static void parser_reply_oom(void) {
    uint8_t frame[80], payload[32] = {0}; usb_parser_t parser = {0};
    size_t bytes = parser_packet(frame, F_CONFIG, payload, sizeof payload);
    unsigned configs = parser_configs, frees = parser_frees;
    parser_expected_len=sizeof payload;
    parser_bytes(&parser, frame, bytes - 1);
    assert(parser.frame && parser.capacity == sizeof payload + 1);
    usb_frame_failures = 1; usb_size = 0;
    parser_bytes(&parser, frame + bytes - 1, 1);
    assert(parser_configs == configs && parser_frees == frees + 1 && !usb_frame_allocation && !usb_chunk_live);
    for (size_t i=0;i<sizeof parsed_request;i++) assert(parsed_request[i]==0);
    parser_error_reply("USB buffer unavailable");
}
static void parser_oom(void) {
    uint8_t payload[64]={0},inner[32];
    const uint8_t request[]="{\"cmd\":\"info\"}";
    size_t inner_len=parser_packet(inner,F_CONFIG,request,sizeof request-1);
    memcpy(payload+3,inner,inner_len); // A complete valid frame inside the failed payload is opaque.
    unsigned configs=parser_configs,routes=parser_routes;
    usb_frame_failures=1;usb_size=0;
    input_frame(F_CONFIG,payload,sizeof payload,true);
    parser_expected_type=F_JSON;parser_expected_len=1;input_frame(F_JSON,payload,1,true);run_usb_rx();
    assert(parser_configs==configs && parser_routes==routes+1 && !usb_frame_allocation);
    parser_error_reply("USB buffer unavailable");
    usb_frame_failures=1;usb_size=0;
    input_frame(F_CONFIG,payload,sizeof payload,false);
    input_frame(F_JSON,payload,1,true);run_usb_rx();
    assert(parser_configs==configs && parser_routes==routes+2 && usb_size==0 && !usb_frame_allocation);
    usb_frame_failures=1;
    input_frame(F_AUDIO,payload,sizeof payload,true);
    input_frame(F_JSON,payload,1,true);run_usb_rx();
    assert(parser_routes==routes+3 && usb_size==0 && !usb_frame_allocation);
}
static void parser_before_deadline(const uint8_t *frame,size_t bytes,size_t payload_len) {
    usb_parser_t parser={0};uint32_t base=time_ms;unsigned configs=parser_configs;
    parser_expected_len=payload_len;
    usb_size=0;parser_bytes(&parser,frame,6);assert(parser.frame);
    time_ms=base+4999;parser_bytes(&parser,frame+6,bytes-6);
    assert(parser_configs==configs+1 && !parser.frame && parser.state==0 && !usb_frame_allocation);
    assert(usb_size==6006 && (usb_bytes[3]|(unsigned)usb_bytes[4]<<8)==6000);
    for(unsigned i=0;i<6000;i++)assert(usb_bytes[5+i]=='r');
    assert(link_usb_crc8(0,usb_bytes+5,6000)==usb_bytes[6005]);
}
static void parser_timeout(void) {
    uint8_t payload[64]={0},frame[70],inner[32];usb_parser_t parser={0};
    const uint8_t request[]="{\"cmd\":\"info\"}";
    memcpy(payload+8,inner,parser_packet(inner,F_CONFIG,request,sizeof request-1));
    size_t bytes=parser_packet(frame,F_CONFIG,payload,sizeof payload);
    parser_before_deadline(frame,bytes,sizeof payload);
    // A fresh, independent frame expires at 5000ms even with intervening progress.
    uint32_t base=time_ms;unsigned configs=parser_configs,routes=parser_routes;
    usb_size=0;parser_bytes(&parser,frame,6);assert(parser.frame && parser_wait(&parser)==1000);
    time_ms=base+3000;parser_bytes(&parser,frame+6,1); // Progress must not extend the whole-frame deadline.
    time_ms=base+4999;parser_expire(&parser);assert(parser.frame && parser_wait(&parser)==1);
    time_ms=base+5000;parser_expire(&parser);assert(!parser.frame && parser.state==5 && parser.received==2);
    size_t error_size=usb_size;parser_error_reply("USB frame timeout");
    time_ms++;parser_expire(&parser);assert(usb_size==error_size);
    parser_bytes(&parser,frame+7,bytes-7);
    assert(parser_configs==configs && parser.state==0 && usb_size==error_size);
    parser_expected_type=F_JSON;parser_expected_len=1;
    bytes=parser_packet(frame,F_JSON,payload,1);parser_bytes(&parser,frame,bytes);
    assert(parser_routes==routes+1 && !parser.frame && !usb_frame_allocation);
    // An entire payload without its CRC still owns the buffer until the same deadline.
    bytes=parser_packet(frame,F_CONFIG,payload,1);parser_bytes(&parser,frame,bytes-1);
    assert(parser.state==6 && parser.frame);time_ms+=5001;parser_expire(&parser);
    assert(!parser.frame && parser.state==6);parser_bytes(&parser,frame+bytes-1,1);
    assert(parser.state==0 && parser_configs==configs);
}
static void parser_rejected(void) {
    uint8_t payload[LINK_USB_PAYLOAD_MAX]={0};unsigned allocated=usb_frame_allocations;
    input_frame(F_CONFIG_REPLY,payload,20,true);
    input_frame(F_CONFIG,payload,LINK_JSON_MAX+1,true);run_usb_rx();
    assert(usb_frame_allocations==allocated && !usb_frame_allocation);
    parser_expected_type=F_BIND;parser_expected_len=0;
    input_frame(F_BIND,payload,0,true);run_usb_rx();
    assert(usb_frame_last_bytes==1 && !usb_frame_allocation);
}
static void usb_parser_cases(void) {
    link_usb_handlers_t saved=s_handlers;
    s_handlers=(link_usb_handlers_t){.on_config_parse=parser_parse,.on_config_info=parser_info,.on_config_reply=parser_config,.on_config_free=parser_free,.on_routed=parser_routed,.on_host_hello=parser_hello};
    parser_stream_info();
    parser_limits();parser_oom();parser_reply_oom();parser_timeout();parser_rejected();s_handlers=saved;
    assert(!usb_frame_allocation && usb_frame_allocations==usb_frame_frees);
    puts("USB parser: lazy exact capacities, alias reply lifetime, scrub/free, CRC/OOM exact drain and whole-frame 5s deadline passed");
}
