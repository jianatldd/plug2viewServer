/* Camera stream plus two persistent switches and eleven momentary preset switches.
 * IDs are stable across firmware upgrades. Preset success acknowledges activation;
 * the motor may still be moving. The trigger immediately resets to off. */
static const struct { const char *id,*name; int preset; } google_devices[]={
    {"plug2view-motion","Plug2View Motion Detection",-1},
    {"plug2view-recording","Plug2View Recording",-1},
    {"plug2view-original","Plug2View Original",0},
    {"plug2view-left-1","Plug2View Left 1",1}, {"plug2view-left-2","Plug2View Left 2",2}, {"plug2view-left-3","Plug2View Left 3",3},
    {"plug2view-right-1","Plug2View Right 1",4}, {"plug2view-right-2","Plug2View Right 2",5}, {"plug2view-right-3","Plug2View Right 3",6},
    {"plug2view-up-1","Plug2View Up 1",7}, {"plug2view-up-2","Plug2View Up 2",8}, {"plug2view-up-3","Plug2View Up 3",9},
    {"plug2view-down-1","Plug2View Down 1",10}
};
static int google_device_index(const char *id) {
    for(unsigned i=0;i<sizeof google_devices/sizeof *google_devices;i++) if(!strcmp(id,google_devices[i].id)) return (int)i;
    return -1;
}
static int google_append(struct google_response *r,size_t *used,const char *fmt,...) {
    va_list ap; va_start(ap,fmt); int n=vsnprintf(r->body+*used,sizeof r->body-*used,fmt,ap); va_end(ap);
    if(n<0 || (size_t)n>=sizeof r->body-*used) return 0; *used+=n; return 1;
}
static int google_array(struct jp *p,int i,int max) { return i>=0 && p->t[i].type=='[' && p->t[i].count>0 && p->t[i].count<=max; }
static void fulfill(const char *body,int online,struct google_response *r) {
    if(!control_callback) { fulfill_stream(body,online,r); return; }
    struct jp p={.s=body};
    if(value(&p,0)<0) { error(r,400,"invalid_json"); return; } ws(&p);
    int rid=field(&p,0,"requestId"),input=single(&p,field(&p,0,"inputs"));
    if(body[p.pos] || rid<0 || p.t[rid].type!='"' || p.t[rid].end-p.t[rid].start>256 || input<0) { error(r,400,"invalid_request"); return; }
    char id[260]; int length=p.t[rid].end-p.t[rid].start; memcpy(id,body+p.t[rid].start,length); id[length]=0;
    int intent=field(&p,input,"intent"), payload=field(&p,input,"payload"); size_t used=0;
#define ADD(...) do { if(!google_append(r,&used,__VA_ARGS__)) { error(r,500,"response_too_large"); return; } } while(0)
    if(jeq(&p,intent,"action.devices.SYNC")) {
        fulfill_stream(body,online,r);
        used=strlen(r->body); if(used<3) { error(r,500,"sync_failed"); return; } used-=3; r->body[used]=0;
        for(unsigned i=0;i<sizeof google_devices/sizeof *google_devices;i++) {
            ADD(",{\"id\":\"%s\",\"type\":\"action.devices.types.SWITCH\",\"traits\":[\"action.devices.traits.OnOff\"],\"name\":{\"name\":\"%s\"},\"willReportState\":false,\"attributes\":{\"commandOnlyOnOff\":false,\"queryOnlyOnOff\":false},\"deviceInfo\":{\"manufacturer\":\"Plug2View\",\"model\":\"Camera control\"}}",google_devices[i].id,google_devices[i].name);
        }
        ADD("]}}"); return;
    }
    if(jeq(&p,intent,"action.devices.QUERY")) {
        int devices=field(&p,payload,"devices");
        if(!google_array(&p,devices,16)) { error(r,400,"invalid_devices"); return; }
        /* Validate all IDs before querying any state. */
        for(int d=devices+1;d<p.t[devices].next;d=p.t[d].next) { char name[65],quoted[132]; if(!jtext(&p,field(&p,d,"id"),name,sizeof name) || !json_quote(name,quoted,sizeof quoted)) { error(r,400,"invalid_device"); return; } }
        ADD("{\"requestId\":%s,\"payload\":{\"devices\":{",id); int first=1;
        for(int d=devices+1;d<p.t[devices].next;d=p.t[d].next) {
            char name[65],quoted[132]; jtext(&p,field(&p,d,"id"),name,sizeof name); json_quote(name,quoted,sizeof quoted);
            int index=google_device_index(name),state=0; const char *err=NULL;
            if(index<0 && strcmp(name,"plug2view")) err="deviceNotFound";
            else if(!online) err="deviceOffline";
            else if(index>=0) err=control_callback(index==0?GOOGLE_MOTION:index==1?GOOGLE_RECORDING:GOOGLE_PRESET,-1,&state);
            ADD("%s%s:{\"online\":%s,\"status\":\"%s\"",first?"":",",quoted,online?"true":"false",err?"ERROR":"SUCCESS"); first=0;
            if(err) ADD(",\"errorCode\":\"%s\"",err);
            else if(index>=0) ADD(",\"on\":%s",index<2 && state?"true":"false");
            ADD("}");
        }
        ADD("}}}"); return;
    }
    if(!jeq(&p,intent,"action.devices.EXECUTE")) { fulfill_stream(body,online,r); return; }
    int commands=field(&p,payload,"commands"),total=0,streams=0;
    if(!google_array(&p,commands,16)) { error(r,400,"invalid_commands"); return; }
    /* Bound batch work and validate shapes before executing any actions. */
    for(int c=commands+1;c<p.t[commands].next;c=p.t[c].next) {
        int devices=field(&p,c,"devices"),execution=field(&p,c,"execution");
        if(!google_array(&p,devices,16) || !google_array(&p,execution,4) || (total+=p.t[devices].count)>16) { error(r,400,"invalid_commands"); return; }
        for(int d=devices+1;d<p.t[devices].next;d=p.t[d].next) {
            char name[65],quoted[132]; if(!jtext(&p,field(&p,d,"id"),name,sizeof name) || !json_quote(name,quoted,sizeof quoted)) { error(r,400,"invalid_device"); return; }
            for(int e=execution+1;e<p.t[execution].next;e=p.t[e].next) {
                int command=field(&p,e,"command");
                if(command<0 || p.t[command].type!='"' || field(&p,e,"params")<0) { error(r,400,"invalid_execution"); return; }
                if(!strcmp(name,"plug2view") && jeq(&p,command,"action.devices.commands.GetCameraStream")) {
                    if(++streams>1 || p.t[execution].count!=1 || p.t[e].end-p.t[e].start>1500) { error(r,400,"invalid_stream_batch"); return; }
                }
            }
        }
    }
    ADD("{\"requestId\":%s,\"payload\":{\"commands\":[",id); int first=1;
    for(int c=commands+1;c<p.t[commands].next;c=p.t[c].next) {
        int devices=field(&p,c,"devices"),execution=field(&p,c,"execution");
        for(int d=devices+1;d<p.t[devices].next;d=p.t[d].next) {
            char name[65],quoted[132]; jtext(&p,field(&p,d,"id"),name,sizeof name); json_quote(name,quoted,sizeof quoted);
            int index=google_device_index(name),state=0; const char *err=NULL;
            int e=execution+1;
            if(!strcmp(name,"plug2view") && jeq(&p,field(&p,e,"command"),"action.devices.commands.GetCameraStream")) {
                char request[2048]; struct google_response response={.code=200};
                snprintf(request,sizeof request,"{\"requestId\":%s,\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[%.*s]}]}}]}",id,p.t[e].end-p.t[e].start,body+p.t[e].start);
                fulfill_stream(request,online,&response);
                char *result=strstr(response.body,"\"commands\":[");
                if(response.code==200 && result) { result+=12; size_t n=strlen(result); if(n>=3) { ADD("%s%.*s",first?"":",",(int)n-3,result); first=0; continue; } }
                err="hardError";
            } else if(index<0) err=!strcmp(name,"plug2view")?"functionNotSupported":"deviceNotFound";
            else if(!online) err="deviceOffline";
            for(;!err && e<p.t[execution].next;e=p.t[e].next) {
                int command=field(&p,e,"command"),params=field(&p,e,"params");
                if(index<2) {
                    int on=field(&p,params,"on");
                    if(!jeq(&p,command,"action.devices.commands.OnOff")) err="functionNotSupported";
                    else if(on<0 || (p.t[on].type!='t' && p.t[on].type!='f')) err="valueOutOfRange";
                    else err=control_callback(index==0?GOOGLE_MOTION:GOOGLE_RECORDING,p.t[on].type=='t',&state);
                } else if(jeq(&p,command,"action.devices.commands.OnOff")) {
                    int on=field(&p,params,"on");
                    if(on<0 || (p.t[on].type!='t' && p.t[on].type!='f')) err="valueOutOfRange";
                    else if(p.t[on].type=='t') err=control_callback(GOOGLE_PRESET,google_devices[index].preset,&state);
                    /* Off is a no-op, never a reverse move or a motor stop. */
                    state=0;
                } else {
                    /* Accept cached scene commands during migration to switches. */
                    int off=field(&p,params,"deactivate");
                    if(!jeq(&p,command,"action.devices.commands.ActivateScene")) err="functionNotSupported";
                    else if(off<0 || p.t[off].type!='f') err="functionNotSupported";
                    else err=control_callback(GOOGLE_PRESET,google_devices[index].preset,&state);
                }
            }
            ADD("%s{\"ids\":[%s],\"status\":\"%s\"",first?"":",",quoted,err?"ERROR":"SUCCESS"); first=0;
            if(err) ADD(",\"errorCode\":\"%s\"",err);
            else ADD(",\"states\":{\"online\":true,\"on\":%s}",index<2 && state?"true":"false");
            ADD("}");
        }
    }
    ADD("]}}");
#undef ADD
}
