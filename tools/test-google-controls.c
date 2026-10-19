/* Exercise the actual fulfillment parser/serializer with isolated controls.
 * Does not load real config, rotate account tokens, or operate the hardware. */
/* Zig's optimized builds define NDEBUG; checks must run in this test binary. */
#undef NDEBUG
#include <assert.h>
#include "../camera/src/google.c"
#include "../camera/src/camera-presets.h"
static int motion,recording,last_preset=-1,calls;
static const char *preset_error;
static const char *control(enum google_control_kind kind,int v,int *state) {
    if(v>=0) calls++;
    if(kind==GOOGLE_MOTION) { if(v>=0) motion=v; *state=motion; }
    else if(kind==GOOGLE_RECORDING) { if(v>=0) recording=v; *state=recording; }
    else { if(v>=0) { if(preset_error) return preset_error; last_preset=v; } *state=0; }
    return NULL;
}
static struct google_response response;
static void run(const char *input,int online) {
    memset(&response,0,sizeof response);response.code=200;
    fulfill(input,online,&response);
    /* SYNC output is larger than the intentionally bounded input parser.
     * The live JS test parses and validates that complete response. */
    if(!strstr(response.body,"\"devices\":[")) {
        struct jp parsed={.s=response.body}; assert(value(&parsed,0)>=0);ws(&parsed);assert(!response.body[parsed.pos]);
    }
}
int main(void) {
    google_set_controls(control);
    run("{\"requestId\":\"test\",\"inputs\":[{\"intent\":\"action.devices.SYNC\"}]}",1);
    int devices=0;const char *cursor=response.body;
    while((cursor=strstr(cursor,"\"id\":"))) { devices++;cursor++; }assert(devices==14);
    assert(!strstr(response.body,"Day") && !strstr(response.body,"Night"));
    assert(!strstr(response.body,"SCENE") && !strstr(response.body,"sceneReversible"));
    int switches=0;
    cursor=response.body;
    while((cursor=strstr(cursor,"\"type\":\"action.devices.types.SWITCH\",\"traits\":[\"action.devices.traits.OnOff\"]"))) { switches++;cursor++; }
    assert(switches==13);
    run("{\"requestId\":\"query\",\"inputs\":[{\"intent\":\"action.devices.QUERY\",\"payload\":{\"devices\":[{\"id\":\"plug2view\"},{\"id\":\"plug2view-motion\"},{\"id\":\"plug2view-recording\"},{\"id\":\"unknown\"}]}}]}",1);
    assert(strstr(response.body,"deviceNotFound") && strstr(response.body,"\"on\":false"));
    const char *batch="{\"requestId\":\"batch\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view-motion\"},{\"id\":\"plug2view-recording\"}],\"execution\":[{\"command\":\"action.devices.commands.OnOff\",\"params\":{\"on\":true}}]}]}}]}";
    run(batch,1); assert(motion && recording && calls==2);
    run(batch,0); assert(calls==2 && strstr(response.body,"deviceOffline"));
    char request[1200];
    for(int i=2;i<13;i++) {
        for(int on=1;on>=0;on--) {
            int before=calls;
            snprintf(request,sizeof request,"{\"requestId\":\"switch\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"%s\"}],\"execution\":[{\"command\":\"action.devices.commands.OnOff\",\"params\":{\"on\":%s}}]}]}}]}",google_devices[i].id,on?"true":"false");
            run(request,1);
            assert(calls==before+on && last_preset==google_devices[i].preset);
            assert(strstr(response.body,"SUCCESS") && strstr(response.body,"\"on\":false"));
            run(request,0); assert(calls==before+on && strstr(response.body,"deviceOffline"));
        }
        snprintf(request,sizeof request,"{\"requestId\":\"query-preset\",\"inputs\":[{\"intent\":\"action.devices.QUERY\",\"payload\":{\"devices\":[{\"id\":\"%s\"}]}}]}",google_devices[i].id);
        run(request,1); assert(strstr(response.body,"\"on\":false"));
    }
    /* A rejected move must not be reported as a successful reset. */
    const char *preset_on="{\"requestId\":\"busy\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view-original\"}],\"execution\":[{\"command\":\"action.devices.commands.OnOff\",\"params\":{\"on\":true}}]}]}}]}";
    preset_error="deviceBusy";run(preset_on,1);
    assert(strstr(response.body,"deviceBusy") && !strstr(response.body,"\"states\""));preset_error=NULL;
    /* Old scene requests remain valid until Google's cached metadata refreshes. */
    for(int i=2;i<14;i++) {
        snprintf(request,sizeof request,"{\"requestId\":\"preset\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"%s\"}],\"execution\":[{\"command\":\"action.devices.commands.ActivateScene\",\"params\":{\"deactivate\":false}}]}]}}]}",i<13?google_devices[i].id:"missing");
        run(request,1); if(i<13) assert(last_preset==google_devices[i].preset); else assert(strstr(response.body,"deviceNotFound"));
    }
    int before=calls;
    run("{\"requestId\":\"invalid\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view-motion\"}],\"execution\":[{\"command\":\"action.devices.commands.OnOff\",\"params\":{\"on\":\"false\"}}]}]}}]}",1);
    assert(calls==before && strstr(response.body,"valueOutOfRange"));
    run("{\"requestId\":\"invalid-preset\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view-original\"}],\"execution\":[{\"command\":\"action.devices.commands.OnOff\",\"params\":{\"on\":\"true\"}}]}]}}]}",1);
    assert(calls==before && strstr(response.body,"valueOutOfRange"));
    run("{\"requestId\":\"invalid\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[],\"execution\":[]}]}}]}",1);
    assert(response.code==400 && calls==before);
    run("{\"requestId\":\"bad-id\",\"inputs\":[{\"intent\":\"action.devices.QUERY\",\"payload\":{\"devices\":[{\"id\":\"\\t\"}]}}]}",1);
    assert(response.code==400 && calls==before);
    /* Streaming retains its original response shape through the batch wrapper. */
    strcpy(g.origin,"https://test.invalid");memset(g.key,'a',64);memset(g.generation,'b',64);
    run("{\"requestId\":\"stream\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.GetCameraStream\",\"params\":{\"StreamToChromecast\":false,\"SupportedStreamProtocols\":[\"hls\"]}}]}]}}]}",1);
    assert(response.code==200 && strstr(response.body,"cameraStreamAccessUrl"));
    int x,y;
    for(int i=0;i<11;i++) { assert(camera_preset_target(i,2020,58,&x,&y));assert(x>=220&&x<=3820&&y>=20&&y<=1057); }
    assert(camera_preset_target(0,2020,58,&x,&y)&&x==2020&&y==58);
    assert(camera_preset_target(3,2020,58,&x,&y)&&x==220&&y==58);
    assert(camera_preset_target(6,2020,58,&x,&y)&&x==3820&&y==58);
    assert(camera_preset_target(9,2020,58,&x,&y)&&x==2020&&y==1057);
    assert(camera_preset_target(10,2020,58,&x,&y)&&x==2020&&y==20);
    assert(!camera_preset_target(11,2020,58,&x,&y));assert(!camera_preset_target(0,-1,58,&x,&y));
    puts("PASS: 14 devices; 13 switches; preset on/reset/off; busy errors; cached scenes; batched controls; QUERY; malformed/unknown/offline handling; stream response; safe preset limits");
    return 0;
}
