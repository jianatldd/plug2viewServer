/* Exercise the actual fulfillment parser/serializer with isolated controls.
 * Does not load real config, rotate account tokens, or operate the hardware. */
/* Zig's optimized builds define NDEBUG; checks must run in this test binary. */
#undef NDEBUG
#include <assert.h>
#include "../camera/src/google.c"
#include "../camera/src/camera-presets.h"
static int motion,recording,last_preset=-1,calls;
static enum google_control_kind last_kind;
static const char *preset_error;
static const char *control(enum google_control_kind kind,int v,int *state) {
    if(v>=0) { calls++;last_kind=kind; }
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
int main(int argc,char **argv) {
    google_set_controls(control);
    run("{\"requestId\":\"test\",\"inputs\":[{\"intent\":\"action.devices.SYNC\"}]}",1);
    if(argc==2 && !strcmp(argv[1],"--sync")) { puts(response.body);return 0; }
    int devices=0;const char *cursor=response.body;
    while((cursor=strstr(cursor,"\"id\":"))) { devices++;cursor++; }assert(devices==1);
    assert(!strstr(response.body,"Day") && !strstr(response.body,"Night"));
    assert(!strstr(response.body,"SCENE") && !strstr(response.body,"sceneReversible"));
    int switches=0;
    cursor=response.body;
    while((cursor=strstr(cursor,"\"type\":\"action.devices.types.SWITCH\",\"traits\":[\"action.devices.traits.OnOff\"]"))) { switches++;cursor++; }
    assert(switches==0);
    assert(strstr(response.body,"action.devices.traits.Toggles") && strstr(response.body,"action.devices.traits.Modes"));
    assert(strstr(response.body,"\"commandOnlyModes\":true") && strstr(response.body,"\"Center\""));
    for(unsigned i=0;i<sizeof google_positions/sizeof *google_positions;i++) assert(strstr(response.body,google_positions[i].name));
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
    /* New actions operate on the camera itself, with truthful toggle queries.
     * Modes acknowledge activation only, since browser moves invalidate targets. */
    const char *toggles="{\"requestId\":\"actions\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.SetToggles\",\"params\":{\"updateToggleSettings\":{\"recording\":false,\"motion_detection\":false}}}]}]}}]}";
    run(toggles,1); assert(!motion && !recording && strstr(response.body,"SUCCESS"));
    assert(strstr(response.body,"\"currentToggleSettings\":{\"motion_detection\":false,\"recording\":false}"));
    before=calls;run(toggles,0); assert(calls==before && strstr(response.body,"deviceOffline"));
    run("{\"requestId\":\"batch-actions\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.SetToggles\",\"params\":{\"updateToggleSettings\":{\"recording\":true}}},{\"command\":\"action.devices.commands.SetModes\",\"params\":{\"updateModeSettings\":{\"position\":\"center\"}}}]}]}}]}",1);
    assert(recording && !motion && last_preset==0 && last_kind==GOOGLE_MOVE && strstr(response.body,"SUCCESS"));
    run("{\"requestId\":\"query-actions\",\"inputs\":[{\"intent\":\"action.devices.QUERY\",\"payload\":{\"devices\":[{\"id\":\"plug2view\"}]}}]}",1);
    assert(strstr(response.body,"\"currentToggleSettings\":{\"motion_detection\":false,\"recording\":true}") && !strstr(response.body,"currentModeSettings"));
    for(unsigned i=0;i<sizeof google_positions/sizeof *google_positions;i++) {
        snprintf(request,sizeof request,"{\"requestId\":\"mode\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.SetModes\",\"params\":{\"updateModeSettings\":{\"position\":\"%s\"}}}]}]}}]}",google_positions[i].name);
        before=calls;run(request,1);assert(calls==before+1 && last_preset==(int)i && last_kind==GOOGLE_MOVE && strstr(response.body,"SUCCESS"));
        assert(!strstr(response.body,"currentModeSettings") && !strstr(response.body,"\"on\""));
        preset_error="deviceBusy";run(request,1);assert(strstr(response.body,"deviceBusy") && !strstr(response.body,"\"states\""));preset_error=NULL;
        before=calls;run(request,0);assert(calls==before && strstr(response.body,"deviceOffline"));
    }
    const char *invalid_settings[]={"{}","{\"recording\":\"true\"}","{\"recording\":true,\"unknown\":false}","[]","true","{\"recording\":null}"};
    for(unsigned i=0;i<sizeof invalid_settings/sizeof *invalid_settings;i++) {
        snprintf(request,sizeof request,"{\"requestId\":\"invalid\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.SetToggles\",\"params\":{\"updateToggleSettings\":%s}}]}]}}]}",invalid_settings[i]);
        before=calls;run(request,1);assert(calls==before && strstr(response.body,"valueOutOfRange"));
    }
    const char *invalid_modes[]={"{}","{\"position\":\"missing\"}","{\"position\":true}","{\"position\":\"center\",\"unknown\":\"left_1\"}","[]"};
    for(unsigned i=0;i<sizeof invalid_modes/sizeof *invalid_modes;i++) {
        snprintf(request,sizeof request,"{\"requestId\":\"invalid\",\"inputs\":[{\"intent\":\"action.devices.EXECUTE\",\"payload\":{\"commands\":[{\"devices\":[{\"id\":\"plug2view\"}],\"execution\":[{\"command\":\"action.devices.commands.SetModes\",\"params\":{\"updateModeSettings\":%s}}]}]}}]}",invalid_modes[i]);
        before=calls;run(request,1);assert(calls==before && strstr(response.body,"valueOutOfRange"));
    }
    /* Match website arrows: repeated jogs accumulate, orthogonal axis stays
     * put, travel clamps, and Center alone returns to the saved initial view. */
    assert(camera_move_target(1,2020,500,2020,58,&x,&y) && x==1900 && y==500);
    assert(camera_move_target(1,x,y,2020,58,&x,&y) && x==1780 && y==500);
    assert(camera_move_target(2,x,y,2020,58,&x,&y) && x==1900 && y==500);
    assert(camera_move_target(3,x,y,2020,58,&x,&y) && x==1900 && y==620);
    assert(camera_move_target(4,x,y,2020,58,&x,&y) && x==1900 && y==500);
    assert(camera_move_target(0,x,y,2020,58,&x,&y) && x==2020 && y==58);
    assert(camera_move_target(1,230,500,0,0,&x,&y) && x==220 && y==500);
    assert(camera_move_target(2,3810,500,0,0,&x,&y) && x==3820 && y==500);
    assert(camera_move_target(3,2020,1050,0,0,&x,&y) && x==2020 && y==1057);
    assert(camera_move_target(4,2020,58,0,0,&x,&y) && x==2020 && y==20);
    assert(!camera_move_target(0,2020,500,-1,58,&x,&y));
    assert(!camera_move_target(5,2020,500,2020,58,&x,&y));
    assert(!camera_move_target(1,-1,500,2020,58,&x,&y));
    puts("PASS: single camera; toggle actions; center + four relative jogs; repeated jogs; travel limits; invalid maps without side effects; offline/busy errors; cached commands; batching; streaming");
    return 0;
}
