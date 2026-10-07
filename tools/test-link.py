#!/usr/bin/env python3
"""Compile the actual transport/state machine with SDK mocks and real cJSON."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
idf = Path(os.environ.get('IDF_PATH', str(Path.home() / 'esp/esp-idf-v5.5.1')))
cjson = idf / 'components/json/cJSON'
def without_includes(path):
    return '\n'.join(line for line in path.read_text().splitlines() if not line.startswith('#include'))
source = '\n'.join(without_includes(root / 'firmware/main' / name)
                   for name in ('ima_adpcm.c', 'link_mic.c', 'link_usb.c', 'link_ws.c', 'link_hello.c', 'link_poke.c', 'link.c'))
source = source.replace('malloc(s_expected + 1)', 'rx_malloc(s_expected + 1)')
mock = (root / 'tools/fixtures/link_stubs.c').read_text()
tests = r'''
static void replace_session_before_send(SemaphoreHandle_t lock) {
    if(lock==s_ws_io_mtx) { before_take=NULL; atomic_fetch_add(&s_session,1); }
}
static char events[128]; static int nevents;
static size_t audio_bytes; static int audio_calls;
static void audio(uint8_t kind,uint8_t tag,const uint8_t *pcm,size_t n) {
    assert(kind==2&&tag==7);assert(pcm[0]==0x55&&pcm[n-1]==0x55);audio_bytes=n;audio_calls++;
}
static void up(bool ready) { events[nevents++]=ready?'U':'D'; }
static void json(const char *p,size_t n) { events[nevents++]=link_is_welcome(p,n)?'W':'J'; }
static int config_calls;
static void config(const char *text,size_t len,char *reply,size_t cap) {
    cJSON *j=cJSON_ParseWithLength(text,len);assert(j);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"cmd")),"info"));
    snprintf(reply,cap,"{\"ok\":true}");cJSON_Delete(j);config_calls++;
}
static void input_frame(uint8_t type,const void *payload,size_t len,bool valid_crc) {
    assert(usb_input_size+len+6<=sizeof usb_input);
    uint8_t *out=usb_input+usb_input_size;out[0]=0xa5;out[1]=0x5a;out[2]=type;
    out[3]=(uint8_t)len;out[4]=(uint8_t)(len>>8);memcpy(out+5,payload,len);
    out[5+len]=link_usb_crc8(0,payload,len)^(valid_crc?0:1);usb_input_size+=len+6;
}
static void run_usb_rx(void) {
    stop_usb_rx=true;
    if(!setjmp(usb_rx_jmp))usb_rx_task(NULL);
    stop_usb_rx=false;usb_input_size=usb_input_pos=0;
}
static void feed_status(const char *s,unsigned epoch) {
    char text[128]; snprintf(text,sizeof text,"{\"v\":2,\"status\":\"%s\",\"epoch\":%u}",s,epoch);
    host_hello(text,strlen(text));
}
static void usb_json(unsigned epoch,const char *text) {
    unsigned char data[512]; epoch_bytes(data,epoch); memcpy(data+4,text,strlen(text));
    usb_routed_input(F_JSON,data,strlen(text)+4);
}
static void usb_bind(unsigned epoch,const char *bind) {
    unsigned char data[80]; epoch_bytes(data,epoch); memcpy(data+4,bind,strlen(bind));
    input_frame(F_BIND,data,strlen(bind)+4,true); run_usb_rx();
}
static char pair_code[32]; static int pairs;
static void pair(const char *code) { snprintf(pair_code,sizeof pair_code,"%s",code); pairs++; }
static void ws_text(const char *text) {
    esp_websocket_event_data_t d={.op_code=1,.payload_len=(int)strlen(text),.data_len=(int)strlen(text),.data_ptr=text};
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&d);
}
static void ws_error(esp_websocket_error_type_t type,int http) {
    esp_websocket_event_data_t d={.error_handle={.error_type=type,.esp_ws_handshake_status_code=http}};
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_ERROR,&d);
}
// Last routed F_JSON frame written to USB at or after `from` (CRC checked).
static const char *last_usb_json(size_t from,uint32_t *epoch) {
    static char text[LINK_JSON_MAX+1]; const char *found=NULL;
    for(size_t i=from;i+6<=usb_size;) {
        assert(usb_bytes[i]==0xA5&&usb_bytes[i+1]==0x5A);
        size_t n=usb_bytes[i+3]|(size_t)usb_bytes[i+4]<<8; assert(i+6+n<=usb_size);
        assert(link_usb_crc8(0,usb_bytes+i+5,n)==usb_bytes[i+5+n]);
        if(usb_bytes[i+2]==F_JSON) { assert(n>=4&&n-4<=LINK_JSON_MAX); *epoch=read_epoch(usb_bytes+i+5);
            memcpy(text,usb_bytes+i+9,n-4); text[n-4]=0; found=text; }
        i+=6+n;
    }
    return found;
}
static void check_hello(const char *text) {
    cJSON *j=cJSON_Parse(text); assert(j);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"t")),"hello"));
    assert(cJSON_GetObjectItem(j,"v")->valuedouble==5);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"device")),"kubik-test"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"key")),"BFakePublicKeyBase64=="));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"fw")),KUBIK_FW_VERSION));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"name")),g_settings.name));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j,"server")),g_settings.server_url));
    assert(cJSON_GetObjectItem(j,"volume")->valueint==g_settings.volume);
    assert(!cJSON_GetObjectItem(j,"token")); // no shared secret on the wire
    cJSON_Delete(j);
}
static size_t idle_bytes;
static uint32_t drop_at,first_usb_ms;
static bool dropped;
// One manager run: a waiting bridge keeps announcing itself, Wi-Fi is lost at drop_at, and the tick where USB is
// first asked for is recorded (every manager_run restarts the task, so the boot grace is measured per run).
static void route_tick(void) {
    atomic_store(&s_host_ms,time_ms);
    if(time_ms<drop_at)assert(usb_size==idle_bytes); // Wi-Fi works: the bridge is left alone
    if(!dropped&&time_ms>=drop_at){dropped=true;wifi_connected=false;ws_event(s_ws,NULL,WEBSOCKET_EVENT_DISCONNECTED,NULL);}
    if(dropped&&!first_usb_ms&&usb_size>idle_bytes)first_usb_ms=time_ms;
}
static void manager_run(int ticks) { manager_ticks=0;manager_limit=ticks;if(!setjmp(manager_jmp))manager_task(NULL); }
int main(void) {
    link_handlers_t handlers={.on_json=json,.on_link=up,.on_audio=audio,.on_pair=pair,.on_config=config}; link_init(&handlers); assert(s_ws_rx==NULL);
    {
        size_t before=usb_size;const char *request="{\"cmd\":\"info\"}";
        input_frame(F_CONFIG,request,strlen(request),true);run_usb_rx();
        assert(config_calls==1&&usb_size>before);
        assert(usb_bytes[before]==0xa5&&usb_bytes[before+1]==0x5a&&usb_bytes[before+2]==F_CONFIG_REPLY);
        size_t payload_len=usb_bytes[before+3]|(size_t)usb_bytes[before+4]<<8;
        assert(payload_len==11&&!memcmp(usb_bytes+before+5,"{\"ok\":true}",payload_len));
        assert(link_usb_crc8(0,usb_bytes+before+5,payload_len)==usb_bytes[before+5+payload_len]);
        before=usb_size;input_frame(F_CONFIG,request,strlen(request),false);run_usb_rx();
        assert(config_calls==1&&usb_size==before);
        usb_size=0; // keep the existing transport scenarios isolated from the config fixture
    }
    strcpy(g_settings.wifi_ssid,"wifi");strcpy(g_settings.server_url,"wss://server");strcpy(g_settings.name,"name \"quoted\"");g_settings.volume=37;
    time_ms=100;
    host_hello("{\"bridge\":1}",12);assert(!link_usb_host_present());
    host_hello("{\"v\":2,\"status\":\"ready\",\"epoch\":0}",36);assert(!link_usb_host_present());
    feed_status("probing",42);assert(link_usb_host_present()&&!link_up());
    wifi_connected=false;manager_run(1);assert(usb_size==0); // Wi-Fi is primary: USB waits out the fallback grace
    link_set_wifi_allowed(false); // no Wi-Fi route wanted: USB probes immediately
    manager_run(1);assert(usb_size>10&&usb_bytes[2]==F_JSON&&read_epoch(usb_bytes+5)==42);
    { uint32_t e=0; const char *h=last_usb_json(0,&e); assert(h&&e==42); check_hello(h); }
    link_set_wifi_allowed(true);wifi_connected=true;
    assert(s_probe_epoch==42&&!link_up()); // direct bootstrap despite no selected route
    const char *welcome="{\"t\":\"welcome\",\"session\":\"usb\"}";
    usb_json(42,welcome);assert(!link_up()); // probing is not authenticated
    feed_status("ready",42);usb_json(41,welcome);assert(!link_up());
    usb_json(42,welcome);assert(!strcmp(link_via(),"usb")&&nevents==2&&events[0]=='U'&&events[1]=='W');
    usb_json(42,"{\"t\":\"state\",\"s\":\"idle\"}");assert(events[2]=='J');
    uint32_t old_session=link_session(); assert(old_session && link_send_json_in_session("{}",old_session));
    uint8_t maximum[LINK_USB_PAYLOAD_MAX+1];memset(maximum,0x55,sizeof maximum);
    epoch_bytes(maximum,42);maximum[4]=2;maximum[5]=7;
    usb_routed_input(F_AUDIO,maximum,LINK_USB_PAYLOAD_MAX);
    assert(audio_calls==1&&audio_bytes==LINK_SPEECH_PCM_MAX);
    usb_routed_input(F_AUDIO,maximum,LINK_USB_PAYLOAD_MAX+1);assert(audio_calls==1);
    int before_json=nevents;
    usb_routed_input(F_JSON,maximum,LINK_USB_EPOCH_BYTES+LINK_JSON_MAX+1);assert(nevents==before_json);
    { // Active capture must forward the original codec bytes, never encode decoded PCM again.
        uint8_t packed[IMA_HEADER_BYTES + LINK_MIC_PCM_MAX/4]; memset(packed,0xa5,sizeof packed); packed[2]=42;
        size_t encoded=0;
        const uint8_t *wire=link_mic_encode(1,old_session,(const int16_t *)maximum,LINK_MIC_PCM_MAX,packed,&encoded);
        assert(encoded==sizeof packed+2 && wire[0]==4 && wire[1]==1 && !memcmp(wire+2,packed,sizeof packed));
    }
    assert(link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX,link_session(),NULL));
    assert(!link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX+1,link_session(),NULL));
    char big_json[LINK_JSON_MAX+2];memset(big_json,' ',sizeof big_json);big_json[sizeof big_json-1]=0;
    assert(!link_send_json(big_json));
    size_t written=usb_size;manager_run(1);assert(usb_size==written); // no duplicate hello on promotion
    feed_status("unavailable",42);assert(!link_up()&&events[3]=='D'&&link_usb_host_present());
    usb_json(42,welcome);assert(!link_up()); // delayed old welcome cannot revive a failed epoch
    atomic_store(&s_host_seen,false);nevents=0;
    assert(ws_start(g_settings.server_url));assert(s_ws_rx==NULL);assert(!link_up());
    char hello[1024];assert(link_make_hello(hello,sizeof hello));assert(strstr(hello,"quoted\\\""));check_hello(hello);
    devkey_ok=false;assert(!link_make_hello(hello,sizeof hello));devkey_ok=true; // no key, no hello
    assert(link_make_hello(hello,sizeof hello));
    s_ws_hello_sent=true;assert(ws_send(hello,strlen(hello),false,true,0));
    assert(!strcmp(link_via(),"wifi")&&events[0]=='U'&&events[1]=='W');
    assert(link_session()!=old_session); int ack_sends=sends;
    assert(!link_send_json_in_session("{}",old_session)&&sends==ack_sends);
    uint32_t current_session=link_session(); assert(link_send_json_in_session("{}",current_session));
    int before_audio = sends;
    assert(!link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX,old_session,NULL) && sends == before_audio);
    assert(link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX,current_session,NULL));
    before_audio=sends; before_take=replace_session_before_send;
    assert(!link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX,link_session(),NULL) && sends==before_audio);
    current_session=link_session(); // Replacement occurred between route snapshot and socket lock.

    uint8_t ws_audio[LINK_WS_RX_MAX+1];memset(ws_audio,0x55,sizeof ws_audio);ws_audio[0]=2;ws_audio[1]=7;
    esp_websocket_event_data_t max_rx={.fin=true,.op_code=2,.payload_len=LINK_WS_RX_MAX,.data_len=LINK_WS_RX_MAX,.data_ptr=(char*)ws_audio};
    fail_rx=true;rx_last_bytes=0;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&max_rx);assert(audio_calls==2&&audio_bytes==LINK_SPEECH_PCM_MAX);
    assert(s_ws_rx==NULL&&rx_last_bytes==0&&link_up()); // complete audio never allocates a duplicate
    max_rx.payload_len=max_rx.data_len=1205;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&max_rx);assert(audio_calls==3&&audio_bytes==1203);
    assert(rx_last_bytes==0&&link_up());fail_rx=false;

    // Non-final frames retain the opcode for continuation after earlier JSON.
    s_ws_rx_op=1;
    esp_websocket_event_data_t continuation=max_rx;continuation.fin=false;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&continuation);
    assert(s_ws_rx_op==2&&audio_calls==4&&rx_last_bytes==1206);
    continuation.op_code=0;continuation.fin=true;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&continuation);
    assert(audio_calls==5&&s_ws_rx==NULL);

    // Real 100ms IMA packet: allocate only its wire size, retaining partial fragments.
    int audio_before=audio_calls;
    esp_websocket_event_data_t part={.op_code=2,.payload_len=1205,.data_len=1024,.data_ptr=(char*)ws_audio};
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(audio_calls==audio_before&&s_ws_rx&&rx_last_bytes==1206);
    part.payload_offset=1024;part.data_len=181;part.data_ptr=(char*)ws_audio+1024;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(audio_calls==audio_before+1&&audio_bytes==1203&&s_ws_rx==NULL);

    // JSON still supports its full 4KiB contract, split across SDK receive callbacks.
    before_json=nevents;
    esp_websocket_event_data_t text_part={.op_code=1,.payload_len=LINK_JSON_MAX,.data_len=1024};
    for (int offset=0;offset<LINK_JSON_MAX;offset+=1024) {
        text_part.payload_offset=offset;text_part.data_ptr=big_json+offset;
        ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&text_part);
        if (offset+1024<LINK_JSON_MAX) assert(nevents==before_json&&s_ws_rx&&rx_last_bytes==LINK_JSON_MAX+1);
    }
    assert(nevents==before_json+1&&s_ws_rx==NULL);

    // A changed declared size or missing prefix cannot write beyond a smaller allocation.
    part.payload_offset=0;part.data_len=1024;part.data_ptr=(char*)ws_audio;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&part);assert(s_ws_rx);
    part.payload_len=LINK_WS_RX_MAX;part.payload_offset=1024;part.data_len=100;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&part);assert(s_ws_rx==NULL);
    part.payload_len=1205;part.payload_offset=100;part.data_len=100;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&part);assert(s_ws_rx==NULL);
    assert(audio_calls==audio_before+1);
    int completed_audio=audio_calls;
    max_rx.payload_len=max_rx.data_len=LINK_WS_RX_MAX+1;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&max_rx);assert(audio_calls==completed_audio);
    max_rx.op_code=1;max_rx.payload_len=max_rx.data_len=LINK_JSON_MAX+1;before_json=nevents;
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&max_rx);assert(nevents==before_json);
    assert(link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX,link_session(),NULL));
    assert(!link_send_mic_in_session(1,(const int16_t *)maximum,LINK_MIC_PCM_MAX+1,link_session(),NULL));
    feed_status("disabled",43);assert(!strcmp(link_via(),"wifi")); // physical bridge does not steal route
    feed_status("ready",44);before_json=nevents;usb_json(44,welcome);
    assert(!strcmp(link_via(),"wifi")&&nevents==before_json); // Wi-Fi is primary: a USB welcome cannot steal it
    ws_stop();assert(s_ws_rx==NULL&&!link_up()&&events[nevents-1]=='D');
    assert(!link_send_json_in_session("{}",current_session));
    feed_status("ready",44);usb_json(44,welcome);
    assert(!strcmp(link_via(),"usb")&&events[nevents-2]=='U'&&events[nevents-1]=='W');
    ws_stop();ws_event((void*)&before_json,NULL,WEBSOCKET_EVENT_DISCONNECTED,NULL);
    assert(!strcmp(link_via(),"usb")); // stale WS stop/disconnect cannot drop USB
    feed_status("disabled",45);nevents=0;
    fail_init=1;assert(!ws_start(g_settings.server_url)&&s_ws==NULL);fail_init=0;
    fail_register=1;assert(!ws_start(g_settings.server_url)&&s_ws==NULL);fail_register=0;
    fail_start=1;assert(!ws_start(g_settings.server_url)&&s_ws==NULL);fail_start=0;
    fail_rx=true;assert(ws_start(g_settings.server_url));assert(s_ws_connected&&!s_ws_failed&&s_ws_rx==NULL);
    esp_websocket_event_data_t unexpected={.op_code=1,.payload_len=2,.data_len=2,.data_ptr="{}"};
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&unexpected); // allocation failure cannot copy to NULL
    assert(!s_ws_connected&&s_ws_failed&&s_ws_rx==NULL);
    ws_stop();fail_rx=false;
    assert(allocations==destroys); // failed clients reclaimed
    atomic_store(&s_host_seen,false);fail_init=1;nattempts=0;time_ms=0;manager_run(300);
    assert(nattempts==8);for(int i=1;i<nattempts;i++)assert(attempts[i]-attempts[i-1]>=2000&&attempts[i]-attempts[i-1]<=10200);
    fail_init=0;nattempts=0;clock_ready=false;strcpy(g_settings.server_url,"wss://server");time_ms=0;manager_run(150);assert(nattempts==0);
    clock_ready=true;manager_run(20);assert(nattempts==1&&link_up());ws_stop();
    assert(allocations==destroys);
    // ---- v5 handshake over WebSocket: hello -> challenge -> auth -> welcome
    strcpy(g_settings.server_url,"wss://server");auto_welcome=false;nevents=0;time_ms=100000;
    int n0=nattempts,d0=destroys,s0=signs,w0=sends;
    manager_run(20);assert(nattempts==n0+1&&s_ws&&s_ws_hello_sent&&!link_up());
    check_hello(ws_last);
    ws_text("{\"t\":\"challenge\",\"nonce\":\"bm9uY2UtMQ==\"}");assert(!link_up()&&signs==s0);
    manager_run(1);assert(signs==s0+1&&!strcmp(signed_nonce,"bm9uY2UtMQ=="));
    assert(!strcmp(ws_last,"{\"t\":\"auth\",\"sig\":\"SIG\"}")&&sends==w0+2&&!strcmp(signed_bind,"ca:gw.example.com"));
    manager_run(1);assert(signs==s0+1&&sends==w0+2); // one auth per challenge
    ws_text("{\"t\":\"welcome\",\"session\":\"wifi\"}");
    assert(!strcmp(link_via(),"wifi")&&link_last_problem()==LINK_OK&&nevents==2&&events[0]=='U'&&events[1]=='W');
    ws_stop();assert(!link_up());
    // ---- routing: Wi-Fi is primary, so a bridge on the cable is never asked for USB while Wi-Fi works;
    // once the session is lost, USB is asked for after USB_DROP_GRACE_MS and not before
    auto_welcome=true;time_ms+=1000;idle_bytes=usb_size;drop_at=time_ms+60000;first_usb_ms=0;
    tick_hook=route_tick;feed_status("probing",90);manager_run(400);tick_hook=NULL;
    assert(dropped&&first_usb_ms-drop_at>=USB_DROP_GRACE_MS&&first_usb_ms-drop_at<USB_DROP_GRACE_MS+400&&s_probe_epoch==90);
    wifi_connected=true;auto_welcome=false;ws_stop();atomic_store(&s_host_seen,false);
    // test hook: a paused Wi-Fi hands over at once and resumes by itself
    auto_welcome=true;time_ms+=1000;manager_run(20);assert(link_up()&&!strcmp(link_via(),"wifi"));
    idle_bytes=usb_size;link_pause_wifi(3000);feed_status("probing",91);manager_run(2);
    assert(!s_ws&&usb_size>idle_bytes&&s_probe_epoch==91);
    feed_status("probing",91);manager_run(20);assert(s_ws&&link_up()); // 4 s later Wi-Fi is back
    auto_welcome=false;ws_stop();atomic_store(&s_host_seen,false);
    // Same running bridge must recover after Wi-Fi takeover, without a new epoch.
    time_ms+=1000;link_pause_wifi(60000);feed_status("ready",92);manager_run(1);usb_json(92,welcome);assert(!strcmp(link_via(),"usb")&&s_probe_epoch==92);
    link_pause_wifi(1);auto_welcome=true;feed_status("ready",92);manager_run(20);
    assert(!strcmp(link_via(),"wifi")&&s_probe_epoch==0&&!s_usb_ready&&!wake_transport_busy);
    idle_bytes=usb_size;link_pause_wifi(60000);feed_status("ready",92);manager_run(2);
    assert(!s_ws&&usb_size>idle_bytes&&s_probe_epoch==92);
    usb_json(92,welcome);assert(!strcmp(link_via(),"usb"));
    idle_bytes=usb_size;feed_status("ready",92);manager_run(1);assert(usb_size==idle_bytes);
    feed_status("disabled",92);link_pause_wifi(1);auto_welcome=false;atomic_store(&s_host_seen,false);
    // ---- an unanswered attempt is abandoned after WSS_ATTEMPT_MS ...
    manager_run(20);assert(s_ws&&s_ws_hello_sent);d0=destroys;
    manager_run(WSS_ATTEMPT_MS/200+10);assert(destroys>d0);ws_stop();
    // ... but not while the owner is approving a pairing code
    manager_run(20);assert(s_ws&&s_ws_hello_sent);
    esp_websocket_client_handle_t pairing_ws=s_ws;n0=nattempts;d0=destroys;nevents=0;
    ws_text("{\"t\":\"pair\",\"code\":\"ABCD2345\"}");
    assert(pairs==1&&!strcmp(pair_code,"ABCD2345")&&link_last_problem()==LINK_PAIRING&&!link_up());
    manager_run(WSS_ATTEMPT_MS/200+20);
    assert(s_ws==pairing_ws&&nattempts==n0&&destroys==d0&&!s_ws_failed);
    ws_text("{\"t\":\"pair\",\"code\":\"ABCD2345\"}");assert(pairs==2); // reminder while waiting
    ws_text("{\"t\":\"welcome\",\"session\":\"paired\"}");
    assert(s_ws==pairing_ws&&!strcmp(link_via(),"wifi")&&link_last_problem()==LINK_OK&&!s_ws_pairing);
    assert(nevents==2&&events[0]=='U'&&events[1]=='W');
    ws_stop();
    // ---- refusals and connection problems
    assert(ws_start(g_settings.server_url) && wake_transport_busy); s_ws_hello_sent=true;
    ws_text("{\"t\":\"error\",\"code\":\"unauthorized\"}");assert(link_last_problem()==LINK_REFUSED&&!link_up());
    ws_stop();
    connect_on_start=false;assert(ws_start(g_settings.server_url)); // HTTP upgrade answered 404: web server, no Kubik plugin
    ws_error(WEBSOCKET_ERROR_TYPE_HANDSHAKE,404);assert(link_last_problem()==LINK_NO_PLUGIN&&s_ws_failed);
    ws_stop();connect_on_start=true;
    assert(ws_start(g_settings.server_url) && wake_transport_busy); s_ws_hello_sent=true;
    esp_websocket_event_data_t close4001={.op_code=8,.payload_len=2,.data_len=2,.data_ptr="\x0f\xa1"};
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&close4001);assert(link_last_problem()==LINK_REFUSED&&!link_up());
    ws_error(WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,0);assert(link_last_problem()==LINK_REFUSED&&s_ws_failed);
    ws_stop();
    assert(ws_start(g_settings.server_url) && wake_transport_busy); s_ws_hello_sent=true;
    esp_websocket_event_data_t close1000={.op_code=8,.payload_len=2,.data_len=2,.data_ptr="\x03\xe8"};
    ws_event(s_ws,NULL,WEBSOCKET_EVENT_DATA,&close1000);assert(link_last_problem()==LINK_REFUSED); // unchanged
    ws_error(WEBSOCKET_ERROR_TYPE_HANDSHAKE,502);assert(link_last_problem()==LINK_NO_PLUGIN);
    ws_stop();
    assert(ws_start(g_settings.server_url)); // TCP failure after connecting is not "no server"
    ws_error(WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,0);assert(link_last_problem()==LINK_NO_PLUGIN);ws_stop();
    connect_on_start=false;assert(ws_start(g_settings.server_url)); // DNS/TCP/TLS failure before connecting
    ws_error(WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,0);assert(link_last_problem()==LINK_NO_SERVER&&s_ws_failed);
    ws_stop();connect_on_start=true;
    assert(allocations==destroys);
    // ---- v5 device handshake over USB: routed challenge answered with the same epoch
    link_set_wifi_allowed(false);nevents=0;w0=sends;s0=signs;
    feed_status("ready",60);size_t u0=usb_size;manager_run(1);
    { uint32_t e=0; const char *h=last_usb_json(u0,&e); assert(h&&e==60); check_hello(h); }
    usb_json(59,"{\"t\":\"challenge\",\"nonce\":\"c3RhbGU=\"}"); // other epoch: ignored
    u0=usb_size;manager_run(1);assert(signs==s0&&usb_size==u0);
    usb_json(60,"{\"t\":\"challenge\",\"nonce\":\"dXNiLW5vbmNl\"}");assert(!link_up());
    feed_status("ready",60);u0=usb_size;manager_run(1);assert(signs==s0&&usb_size==u0); // no bind for 60 yet: no auth
    usb_bind(59,"ca:gw.example.com");usb_bind(60,"ca:gw.example.com");
    usb_json(60,"{\"t\":\"challenge\",\"nonce\":\"dXNiLW5vbmNl\"}");
    feed_status("ready",60);u0=usb_size;manager_run(1);
    assert(signs==s0+1&&!strcmp(signed_nonce,"dXNiLW5vbmNl")&&!strcmp(signed_bind,"ca:gw.example.com")&&sends==w0); // over USB, not the WebSocket
    { uint32_t e=0; const char *a=last_usb_json(u0,&e); assert(a&&e==60&&!strcmp(a,"{\"t\":\"auth\",\"sig\":\"SIG\"}")); }
    usb_json(60,"{\"t\":\"pair\",\"code\":\"USBC0DE1\"}");
    assert(pairs==3&&!strcmp(pair_code,"USBC0DE1")&&link_last_problem()==LINK_PAIRING&&!link_up());
    usb_json(60,"{\"t\":\"welcome\",\"session\":\"usb2\"}");
    assert(!strcmp(link_via(),"usb")&&link_last_problem()==LINK_OK&&nevents==2&&events[0]=='U'&&events[1]=='W');
    // docs/protocol.md + tools/usb-bridge: the bridge relays the challenge while its
    // heartbeat still says "probing" (it turns "ready" only on the welcome).
    assert(keeps_usb>0); // the welcome lets LAN mode keep the reported key
    feed_status("probing",61);manager_run(1);s0=signs;usb_bind(61,"ca:gw.example.com");
    usb_json(61,"{\"t\":\"challenge\",\"nonce\":\"cHJvYmluZw==\"}");feed_status("probing",61);manager_run(1);
    assert(signs==s0+1&&!strcmp(signed_nonce,"cHJvYmluZw==")&&!link_up());
    { uint32_t e=0; const char *a=last_usb_json(u0,&e); assert(a&&e==61&&!strcmp(a,"{\"t\":\"auth\",\"sig\":\"SIG\"}")); }
    s0=signs;
    usb_json(60,"{\"t\":\"challenge\",\"nonce\":\"c3RhbGU=\"}");manager_run(1);
    assert(signs==s0); // a stale epoch cannot trigger a signature
    feed_status("unavailable",61);
    usb_json(61,"{\"t\":\"challenge\",\"nonce\":\"bm90LXJlYWR5\"}");manager_run(1);
    assert(signs==s0&&!link_up()); // unavailable bridges cannot initiate handshakes
    link_set_wifi_allowed(true);atomic_store(&s_host_seen,false);time_ms+=5000;manager_run(1);
    assert(!link_up());ws_stop();
    assert(allocations==destroys);
    // ---- v5 LAN mode: discovery, pinned TLS without a clock, the binding in the signature
    strcpy(g_settings.server_url,"");clock_ready=false;auto_welcome=false;nevents=0;time_ms+=60000;
    atomic_store(&s_problem,LINK_OK);target_result=TARGET_WAITING;n0=nattempts;
    manager_run(20);assert(nattempts==n0&&!s_ws); // still looking for the plugin
    target_result=TARGET_NOT_FOUND;manager_run(20);assert(link_last_problem()==LINK_NO_SERVER&&nattempts==n0);
    target_result=TARGET_READY;manager_run(100);
    assert(nattempts==n0+1&&!strcmp(last_uri,"wss://10.0.0.5:18790/kubik/v1")&&last_attach==link_pin_attach);
    assert(s_ws_hello_sent);check_hello(ws_last);
    s0=signs;ws_text("{\"t\":\"challenge\",\"nonce\":\"bGFu\"}");manager_run(1);
    assert(signs==s0+1&&!strcmp(signed_bind,"0123abcd"));
    int k0=keeps_wifi;manager_run(1);assert(keeps_wifi==k0); // no welcome yet: nothing is pinned
    ws_text("{\"t\":\"welcome\",\"session\":\"lan\"}");manager_run(1);
    assert(!strcmp(link_via(),"wifi")&&keeps_wifi>k0);
    // Pokes are noted by any task and sent from the manager: at most one a second, the most telling of those noted.
    {
        time_ms+=2000;manager_run(1);int w=sends;
        link_poke(POKE_TAP);link_poke(POKE_PET);link_poke(POKE_TAP);assert(sends==w);   // noting sends nothing
        manager_run(1);assert(sends==w+1&&!strcmp(ws_last,"{\"t\":\"poke\",\"kind\":\"pet\"}"));
        link_poke(POKE_SHAKE);manager_run(1);assert(sends==w+1);                          // within a second: held back
        time_ms+=1000;manager_run(1);assert(sends==w+2&&!strcmp(ws_last,"{\"t\":\"poke\",\"kind\":\"shake\"}"));
        manager_run(1);assert(sends==w+2);                                                  // nothing noted, nothing sent
    }
    int t0=target_stops;wifi_connected=false;manager_run(1);assert(!link_up()&&target_stops>t0);wifi_connected=true;
    // A different key fails TLS; the screen says so and later failures do not hide it.
    char retry_uri[sizeof last_uri];snprintf(retry_uri,sizeof retry_uri,"%s",last_uri);
    assert(!strcmp(retry_uri,last_uri));connect_on_start=false;assert(ws_start(retry_uri));pin_mismatch=true;
    ws_error(WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,0);manager_run(1);assert(link_last_problem()==LINK_KEY_CHANGED);
    ws_stop();assert(ws_start(retry_uri));ws_error(WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,0);
    assert(link_last_problem()==LINK_KEY_CHANGED);ws_stop();connect_on_start=true;
    target_result=TARGET_NOT_FOUND;time_ms+=60000;manager_run(20);assert(link_last_problem()==LINK_KEY_CHANGED);
    target_result=TARGET_READY;
    strcpy(g_settings.server_url,"kubik://mac.local");assert(!ws_start(g_settings.server_url));assert(ws_start("wss://mac.local:18790/kubik/v1")&&last_attach==link_pin_attach);
    char endpoint[160]; atomic_store(&s_via,VIA_WIFI); link_endpoint(endpoint,sizeof endpoint);
    assert(!strcmp(endpoint,"wss://mac.local:18790/kubik/v1"));
    atomic_store(&s_via,VIA_NONE); link_endpoint(endpoint,sizeof endpoint); assert(!endpoint[0]); ws_stop();
    strcpy(g_settings.server_url,"wss://server");assert(ws_start(g_settings.server_url)&&last_attach==esp_crt_bundle_attach);ws_stop();
    // Transient poll failure can retry, but respects backoff and a fixed attempt cap.
    clock_ready=true;poll_mode=true;poll_attempts=0;fail_start=1;n0=nattempts;
    manager_run(200);assert(nattempts==n0+3&&!s_ws&&poll_attempts==3);
    poll_attempts=0;manager_run(200);assert(nattempts==n0+6);
    poll_attempts=0;manager_retry_t recovery={.boot=time_ms-10000,.backoff=1000};
    manager_state_t snapshot={.now=time_ms,.wifi_ok=true};
    manager_connect(&snapshot,&recovery);assert(poll_attempts==1&&!s_ws);
    assert(!manager_ws_due(&snapshot,&recovery));
    fail_start=0;time_ms=recovery.next_ws;snapshot.now=time_ms;
    assert(manager_ws_due(&snapshot,&recovery));manager_connect(&snapshot,&recovery);
    assert(poll_attempts==2&&s_ws);ws_stop();
    poll_mode=false;
    // A captured allowed=true snapshot cannot create a socket after the caller closed admission.
    manager_state_t stale={.now=time_ms,.wifi_ok=true}; manager_retry_t retry={.boot=time_ms-10000};
    link_set_wifi_allowed(false); n0=nattempts; manager_connect(&stale,&retry); assert(nattempts==n0&&!s_ws);
    link_set_wifi_allowed(true); stop_during_ws_start=true; manager_connect(&stale,&retry);
    assert(!s_ws&&!atomic_load(&s_wifi_allowed));
    // Over USB a key that contradicts the pin leaves no binding, so no auth.
    link_set_wifi_allowed(false);feed_status("probing",70);manager_run(1);s0=signs;
    usb_bind(70,"bad");manager_run(1);assert(link_last_problem()==LINK_KEY_CHANGED);
    usb_json(70,"{\"t\":\"challenge\",\"nonce\":\"a2V5\"}");feed_status("probing",70);manager_run(1);assert(signs==s0);
    usb_bind(70,"0123abcd");usb_json(70,"{\"t\":\"challenge\",\"nonce\":\"a2V5\"}");feed_status("probing",70);manager_run(1);
    assert(signs==s0+1&&!strcmp(signed_bind,"0123abcd"));
    link_set_wifi_allowed(true);atomic_store(&s_host_seen,false);manager_run(1);ws_stop();
    assert(allocations==destroys);
    assert(link_usb_crc8(0,(const uint8_t*)"123456789",9)==0xf4);
    assert(s_ws_rx==NULL);
    puts("link: strict v5 device protocol and v2 USB transport, epoch routing, immediate welcome ordering, WS cleanup/backoff, WSS time gating, challenge/auth with binding over WS and USB (0x23), LAN discovery/pinning/key-changed, pairing hold, refusal/no-plugin/no-server passed");
}
'''
with tempfile.TemporaryDirectory(prefix='c6-link-test-') as tmp:
    src=Path(tmp)/'test.c';exe=Path(tmp)/'test';src.write_text(mock+source+tests)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-deprecated-declarations',
        '-fsanitize=address,undefined','-g','-O1','-I'+str(cjson),'-I'+str(root/'firmware/main'),'-I'+str(root/'firmware/sim/settings_stubs'),str(src),str(cjson/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
