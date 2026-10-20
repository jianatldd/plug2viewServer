/* camd-side control callback. Recording stays on the capture thread; slow motor
 * movement runs in hkctl, sharing its lock with the browser control API. */
static pid_t google_preset_pid;
static int google_last_preset=-1;
static int control_position(const char *path,int *x,int *y) {
    FILE *f=fopen(path,"r"); if(!f) return 0;
    int ok=fscanf(f,"%d %d",x,y)==2; fclose(f); return ok;
}
static const char *google_camera_control(enum google_control_kind kind,int value,int *state) {
    *state=0;
    if(kind==GOOGLE_RECORDING) {
        rec_expire();
        if(value==1 && !rec_manual) {
            char name[256]; if(rec_start("manual",1,name,sizeof name)) return "hardError";
        } else if(value==0 && rec_manual) rec_stop();
        *state=rec_fd>=0 && rec_manual; return NULL;
    }
    if(kind==GOOGLE_MOTION) {
        const char *file="/mnt/sif/owrt/motion.conf", *tmp="/mnt/sif/owrt/motion.conf.google.tmp";
        char keep[1024]="",line[128]; size_t used=0; int on=0;
        FILE *f=fopen(file,"r");
        if(f) {
            while(fgets(line,sizeof line,f)) {
                if(!strncmp(line,"MOTION=",7)) { on=atoi(line+7)!=0; continue; }
                size_t n=strlen(line); if(used+n>=sizeof keep) { fclose(f); return "hardError"; }
                memcpy(keep+used,line,n+1); used+=n;
            }
            int bad=ferror(f); fclose(f); if(bad) return "hardError";
        } else if(errno!=ENOENT) return "hardError";
        else strcpy(keep,"THRESHOLD=273\nZONES=511\nCONFIRM=2\n");
        if(value>=0 && value!=on) {
            f=fopen(tmp,"w"); if(!f) return "hardError";
            int ok=fprintf(f,"MOTION=%d\n%s",value,keep)>=0;
            if(fclose(f)) ok=0;
            if(!ok || rename(tmp,file)) { remove(tmp); return "hardError"; }
            on=value;
        }
        *state=on; return NULL;
    }
    if(kind==GOOGLE_PRESET || kind==GOOGLE_MOVE) {
        int x,y,px=0,py=0,tx,ty;
        if(!control_position("/tmp/neck.pos",&x,&y) && !control_position("/mnt/sif/owrt/neck.pos",&x,&y)) return "deviceNotReady";
        if((kind==GOOGLE_PRESET || value<=0) && !control_position("/mnt/sif/owrt/park.pos",&px,&py)) return "deviceNotReady";
        int valid=kind==GOOGLE_MOVE?camera_move_target(value<0?0:value,x,y,px,py,&tx,&ty):camera_preset_target(value<0?0:value,px,py,&tx,&ty);
        if(!valid || x<0 || x>4040 || y<0 || y>1077) return "deviceNotReady";
        if(access("/mnt/sif/owrt/bin/hkctl",X_OK)) return "deviceOffline";
        if(value<0) return NULL;
        if(google_preset_pid>0 && kill(google_preset_pid,0)==0) return kind==GOOGLE_PRESET && value==google_last_preset?NULL:"deviceBusy";
        int lock=open("/tmp/neck.lock",O_RDWR|O_CREAT,0644);
        if(lock<0) return "hardError";
        if(flock(lock,LOCK_EX|LOCK_NB)) { close(lock); return "deviceBusy"; }
        flock(lock,LOCK_UN); close(lock);
        char index[12]; snprintf(index,sizeof index,"%d",value);
        char *args[]={"hkctl","neck",kind==GOOGLE_MOVE?"move":"preset",index,NULL};
        pid_t pid=fork(); if(pid<0) return "hardError";
        if(!pid) {
            for(int fd=3;fd<1024;fd++) close(fd);
            execv("/mnt/sif/owrt/bin/hkctl",args); _exit(127);
        }
        google_preset_pid=pid; google_last_preset=kind==GOOGLE_PRESET?value:-1;
        fprintf(stderr,"google: %s activated index=%d target=%d,%d\n",kind==GOOGLE_MOVE?"move":"preset",value,tx,ty);
        return NULL;
    }
    return "functionNotSupported";
}
