#include "box.h"        // TODO Remove need
#include <opencv2/core/core_c.h>

/* Object detection API */
int net_parse_arguments(int argc, char **argv);
void net_prepare(void);
void log_detections(int w, int h, int num, float thresh, box *boxes, float **probs, char **names, int classes);
void *net_get_image_data(IplImage *cv_image);
void net_free_image_data(void *image_data);
int net_process_image(void *image_data, float thresh);
