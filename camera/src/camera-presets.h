#ifndef P2V_CAMERA_PRESETS_H
#define P2V_CAMERA_PRESETS_H
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
