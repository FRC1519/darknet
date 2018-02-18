#include "box.h"        // TODO Remove need
#include "image.h"      // TODO Remove need

/* Object detection API */
int net_parse_arguments(int argc, char **argv);
void net_prepare(void);
void log_detections(int w, int h, int num, float thresh, box *boxes, float **probs, char **names, int classes);
int net_process_image(image img, float thresh);
