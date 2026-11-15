/* S20 lens selection; nominal0.6 UI scale, not a measured focal calibration. */
#ifndef CAMERA_S20_ZOOM_H
#define CAMERA_S20_ZOOM_H
#include <stdbool.h>
#include <math.h>
static inline float s20_zoom_clamp(float z,bool rear,bool enabled){
    float min=rear&&enabled?0.6f:1.0f;
    if(!isfinite(z))return 1.0f;
    return z<min?min:z>16?16:z;
}
static inline unsigned s20_zoom_mode(float z,bool enabled){return enabled&&z<1?5:2;}
static inline float s20_zoom_crop(float z,bool ultra){return ultra?z/0.6f:z;}
#endif
