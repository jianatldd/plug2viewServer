/* Legacy IDs remain executable while Google refreshes cached metadata. Only the
 * camera is advertised; its toggles and position mode own all new commands. */
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
static const struct { const char *name,*synonyms; } google_positions[]={
    {"center","\"Center\",\"Centre\",\"Original\",\"Home\""},
    {"left","\"Left\",\"Look left\""},
    {"right","\"Right\",\"Look right\""},
    {"up","\"Up\",\"Look up\""},
    {"down","\"Down\",\"Look down\""}
};
static const char *google_toggle_state(int *motion,int *recording) {
    const char *err=control_callback(GOOGLE_MOTION,-1,motion);
    return err?err:control_callback(GOOGLE_RECORDING,-1,recording);
}
static const char *google_camera_action(struct jp *p,int command,int params) {
    int state=0;
    if(jeq(p,command,"action.devices.commands.SetToggles")) {
        int settings=field(p,params,"updateToggleSettings");
        if(settings<0 || p->t[settings].type!='{' || !p->t[settings].count) return "valueOutOfRange";
        /* Validate the whole map before changing either toggle. */
        for(int k=settings+1;k<p->t[settings].next;k=p->t[p->t[k].next].next) {
            int v=p->t[k].next;
            if(!jeq(p,k,"motion_detection") && !jeq(p,k,"recording")) return "valueOutOfRange";
            if(p->t[v].type!='t' && p->t[v].type!='f') return "valueOutOfRange";
        }
        for(int k=settings+1;k<p->t[settings].next;k=p->t[p->t[k].next].next) {
            const char *err=control_callback(jeq(p,k,"recording")?GOOGLE_RECORDING:GOOGLE_MOTION,p->t[p->t[k].next].type=='t',&state);
            if(err) return err;
        }
        return NULL;
    }
    if(jeq(p,command,"action.devices.commands.SetModes")) {
        int settings=field(p,params,"updateModeSettings"),position=field(p,settings,"position");
        if(settings<0 || p->t[settings].type!='{' || p->t[settings].count!=1 || position<0) return "valueOutOfRange";
        for(unsigned i=0;i<sizeof google_positions/sizeof *google_positions;i++)
            if(jeq(p,position,google_positions[i].name)) return control_callback(GOOGLE_MOVE,(int)i,&state);
        return "valueOutOfRange";
    }
    return "functionNotSupported";
}
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
        ADD("{\"requestId\":%s,\"payload\":{\"agentUserId\":\"plug2view-owner\",\"devices\":[{\"id\":\"plug2view\",\"type\":\"action.devices.types.CAMERA\",\"traits\":[\"action.devices.traits.CameraStream\",\"action.devices.traits.Toggles\",\"action.devices.traits.Modes\"],\"name\":{\"name\":\"Plug2View Camera\",\"nicknames\":[\"Camera\"]},\"willReportState\":false,\"attributes\":{\"cameraStreamSupportedProtocols\":[%s\"hls\"],\"cameraStreamNeedAuthToken\":false,",id,rtc_ready()?"\"webrtc\",":"");
        ADD("\"availableToggles\":[{\"name\":\"recording\",\"name_values\":[{\"name_synonym\":[\"Recording\",\"Manual recording\"],\"lang\":\"en\"}]},{\"name\":\"motion_detection\",\"name_values\":[{\"name_synonym\":[\"Motion detection\",\"Motion recording\"],\"lang\":\"en\"}]}],\"commandOnlyToggles\":false,\"queryOnlyToggles\":false,");
        ADD("\"availableModes\":[{\"name\":\"position\",\"name_values\":[{\"name_synonym\":[\"Position\",\"View\",\"Direction\"],\"lang\":\"en\"}],\"settings\":[");
        for(unsigned i=0;i<sizeof google_positions/sizeof *google_positions;i++)
            ADD("%s{\"setting_name\":\"%s\",\"setting_values\":[{\"setting_synonym\":[%s],\"lang\":\"en\"}]}",i?",":"",google_positions[i].name,google_positions[i].synonyms);
        /* Viewer moves can invalidate a previously requested preset. Do not
         * claim a queried mode or completed movement from a cached target. */
        ADD("],\"ordered\":false}],\"commandOnlyModes\":true,\"queryOnlyModes\":false},\"deviceInfo\":{\"manufacturer\":\"Plug2View\",\"model\":\"HK camera\",\"swVersion\":\"experimental-actions-1\"}}]}}"); return;
    }
    if(jeq(&p,intent,"action.devices.QUERY")) {
        int devices=field(&p,payload,"devices");
        if(!google_array(&p,devices,16)) { error(r,400,"invalid_devices"); return; }
        /* Validate all IDs before querying any state. */
        for(int d=devices+1;d<p.t[devices].next;d=p.t[d].next) { char name[65],quoted[132]; if(!jtext(&p,field(&p,d,"id"),name,sizeof name) || !json_quote(name,quoted,sizeof quoted)) { error(r,400,"invalid_device"); return; } }
        ADD("{\"requestId\":%s,\"payload\":{\"devices\":{",id); int first=1;
        for(int d=devices+1;d<p.t[devices].next;d=p.t[d].next) {
            char name[65],quoted[132]; jtext(&p,field(&p,d,"id"),name,sizeof name); json_quote(name,quoted,sizeof quoted);
            int index=google_device_index(name),state=0,motion=0,recording=0; const char *err=NULL;
            if(index<0 && strcmp(name,"plug2view")) err="deviceNotFound";
            else if(!online) err="deviceOffline";
            else if(index>=0) err=control_callback(index==0?GOOGLE_MOTION:index==1?GOOGLE_RECORDING:GOOGLE_PRESET,-1,&state);
            else err=google_toggle_state(&motion,&recording);
            ADD("%s%s:{\"online\":%s,\"status\":\"%s\"",first?"":",",quoted,online?"true":"false",err?"ERROR":"SUCCESS"); first=0;
            if(err) ADD(",\"errorCode\":\"%s\"",err);
            else if(index>=0) ADD(",\"on\":%s",index<2 && state?"true":"false");
            else ADD(",\"currentToggleSettings\":{\"motion_detection\":%s,\"recording\":%s}",motion?"true":"false",recording?"true":"false");
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
            int index=google_device_index(name),state=0,motion=0,recording=0; const char *err=NULL;
            int e=execution+1;
            if(!strcmp(name,"plug2view") && jeq(&p,field(&p,e,"command"),"action.devices.commands.GetCameraStream")) {
                char request[2048]; struct google_response response={.code=200};
                snprintf(request,sizeof request,"{\"requestId\":%s,\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[%.*s]}]}}]}",id,p.t[e].end-p.t[e].start,body+p.t[e].start);
                fulfill_stream(request,online,&response);
                char *result=strstr(response.body,"\"commands\":[");
                if(response.code==200 && result) { result+=12; size_t n=strlen(result); if(n>=3) { ADD("%s%.*s",first?"":",",(int)n-3,result); first=0; continue; } }
                err="hardError";
            } else if(index<0 && strcmp(name,"plug2view")) err="deviceNotFound";
            else if(!online) err="deviceOffline";
            for(;!err && e<p.t[execution].next;e=p.t[e].next) {
                int command=field(&p,e,"command"),params=field(&p,e,"params");
                if(index<0) err=google_camera_action(&p,command,params);
                else if(index<2) {
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
            if(!err && index<0) err=google_toggle_state(&motion,&recording);
            ADD("%s{\"ids\":[%s],\"status\":\"%s\"",first?"":",",quoted,err?"ERROR":"SUCCESS"); first=0;
            if(err) ADD(",\"errorCode\":\"%s\"",err);
            else if(index>=0) ADD(",\"states\":{\"online\":true,\"on\":%s}",index<2 && state?"true":"false");
            else ADD(",\"states\":{\"online\":true,\"currentToggleSettings\":{\"motion_detection\":%s,\"recording\":%s}}",motion?"true":"false",recording?"true":"false");
            ADD("}");
        }
    }
    ADD("]}}");
#undef ADD
}
