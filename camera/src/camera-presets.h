#ifndef P2V_CAMERA_PRESETS_H
#define P2V_CAMERA_PRESETS_H
/* Match one website arrow press (STEP=120). Resolve relative moves from the
 * current position under the motor lock; only Center uses saved park. */
static int camera_move_target(int direction,int cx,int cy,int px,int py,int *x,int *y) {
    if(direction<0 || direction>4 || cx<0 || cx>4040 || cy<0 || cy>1077) return 0;
    *x=cx; *y=cy;
    if(direction==0) {
        if(px<220 || px>3820 || py<20 || py>1057) return 0;
        *x=px; *y=py;
    } else if(direction==1) *x-=120;
    else if(direction==2) *x+=120;
    else if(direction==3) *y+=120;
    else *y-=120;
    if(*x<220) *x=220; if(*x>3820) *x=3820;
    if(*y<20) *y=20; if(*y>1057) *y=1057;
    return 1;
}
/* Existing calibrated software travel limits. Index 0 returns to saved park;
 * 1..3 left, 4..6 right, 7..9 up, 10 down. Each preset starts from park. */
static int camera_preset_target(int preset,int px,int py,int *x,int *y) {
    if(preset<0 || preset>10 || px<220 || px>3820 || py<20 || py>1057) return 0;
    *x=px; *y=py;
    if(preset>=1 && preset<=3) *x=px-(px-220)*preset/3;
    else if(preset>=4 && preset<=6) *x=px+(3820-px)*(preset-3)/3;
    else if(preset>=7 && preset<=9) *y=py+(1057-py)*(preset-6)/3;
    else if(preset==10) *y=20;
    return 1;
}
#endif
