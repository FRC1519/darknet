/*
 * This code is based on the code that runs "darknet detector demo".
 * It is extracted from code found in darknet.c, detector.c, and demo.c
 */

#include <stdio.h>
#include <pthread.h>
#include <opencv2/core/core_c.h>

#include "robot.h"
#include "darknet.h"
#include "utils.h"
#include "box.h"
#include "image.h"

/* Configuration files */
char *cfgfile;
char *weightfile;

/*
 * Image buffers -- to avoid reallocating for every new frame
 * Only 3 are needed -- one being processed, a new incoming frame, and the old
 * frame being replaced.  The internal buffer will be lazily allocated when
 * first needed, as image properties will be known then.
 */
#define MAX_IMAGES 3
image *free_images[MAX_IMAGES];
pthread_mutex_t free_image_lock = PTHREAD_MUTEX_INITIALIZER;

/* Darknet magic variables... */
network *net;
float **probs;
box *boxes;
int detections = 0;
float **predictions;
float *avg;
char **names;
int classes;
float hier = .5;
int avg_frames = 3;

/* Process remaining command line arguments, which below to the network */
int net_parse_arguments(int argc, char **argv) {
    /* Get names of configuration files */
    if (argc != 3) {
        fprintf(stderr, "expected network arguments: [datacfg] [cfg] [weights]\n");
        return -1;
    }
    char *datacfg = argv[0];
    cfgfile = argv[1];
    weightfile = argv[2];

    /* Read in data config */
    list *options = read_data_cfg(datacfg);
    classes = option_find_int(options, "classes", 20);
    char *name_list = option_find_str(options, "names", NULL);
    if (!name_list) {
        error("Name list not defined in data configuration\n");
    }
    names = get_labels(name_list);

    return 0;
}

/*
 * Prepare the darknet network for processing
 * Code derived from demo() in src/demo.c
 */
void net_prepare(void) {
    /* Load in network from config file and weights */
    predictions = calloc(avg_frames, sizeof(float*));
    net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);

    /* Darknet magic, derived from demo() in src/demo.c */
    layer l = net->layers[net->n-1];
    detections = l.n*l.w*l.h;
    int j;

    avg = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < avg_frames; ++j)
        predictions[j] = (float *) calloc(l.outputs, sizeof(float));

    boxes = (box *)calloc(l.w*l.h*l.n, sizeof(box));
    probs = (float **)calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j)
        probs[j] = (float *) calloc(l.classes+1, sizeof(float));

    /* Pre-allocated image buffers */
    image *images = calloc(MAX_IMAGES, sizeof(image));
    for (j = 0; j < MAX_IMAGES; j++) {
        make_empty_image(images[j]);
        free_images[j] = &images[j];
    }
}

/* Log the detected objects */
void log_detections(int w, int h, int num, float thresh, box *boxes, float **probs, char **names, int classes) {
    int i, j;

    printf("Objects:\n");
    for (i = 0; i < num; ++i) {
        int class = -1;
        float prob = 0.0;

        for (j = 0; j < classes; ++j) {
            if (probs[i][j] > thresh) {
                printf("  %s: %.0f%%\n", names[j], probs[i][j]*100);
                if (probs[i][j] > prob ) {
                    class = j;
                    prob = probs[i][j];
                } else {
                    printf("    --> not better than %s @ %.0f%%\n", names[class], prob);
                }
            }
        }
        if (class >= 0){
            printf("  %d %s: %.0f%%\n", i, names[class], prob*100);

            /* TODO Implement real notification method via callback or data structure*/
#if 0
            box b = boxes[i];

            int left  = (b.x-b.w/2.)*w;
            int right = (b.x+b.w/2.)*w;
            int top   = (b.y-b.h/2.)*h;
            int bot   = (b.y+b.h/2.)*h;

            if(left < 0) left = 0;
            if(right > w-1) right = w-1;
            if(top < 0) top = 0;
            if(bot > h-1) bot = h-1;
#endif
        }
    }
}

/*
 * Get the image data that darknet needs from the OpenCV image
 * c.f. get_image_from_stream and fill_image_from_stream
 */
void *net_get_image_data(IplImage *cv_image) {
    image *img = NULL;

    /* Find free image buffer */
    pthread_mutex_lock(&free_image_lock);
    for (int j = 0; j < MAX_IMAGES; j++) {
        if (free_images[j] != NULL) {
            img = free_images[j];
            free_images[j] = NULL;
        }
    }
    pthread_mutex_unlock(&free_image_lock);
    assert(img != NULL);

    /* Copy data from OpenCV image into network-appropriate buffer */
    if (img->data == NULL)
        *img = ipl_to_image(cv_image);
    else
        ipl_into_image(cv_image, *img);
    rgbgr_image(*img);

    /* Return image buffer opaquely */
    return img;
}

/*
 * Return image data back onto the free list
 */
void net_free_image_data(void *image_data) {
    image *img = image_data;

    /* Quick return if called with NULL pointer, though that shouldn't happen */
    if (img == NULL)
        return;

    /* Add image buffer back to the list of free image buffers */
    pthread_mutex_lock(&free_image_lock);
    for (int j = 0; j < MAX_IMAGES; j++) {
        if (free_images[j] == NULL) {
            free_images[j] = img;
            img = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&free_image_lock);
    assert(img == NULL);
}

/*
 * Do the network-specific processing of a new image, and notify if objects are
 * found about the specified threshold
 */
int net_process_image(void *image_data, float thresh) {
    static image boxed_image = { 0 };
    static int index = 0;
    image *img = image_data;

    /* Letterbox image to match the size of the network */
    if (boxed_image.data == NULL)
        boxed_image = letterbox_image(*img, net->w, net->h);
    else
        letterbox_image_into(*img, net->w, net->h, boxed_image);

    /*
     * The following Darknet magic is derived from detect_in_thread() in
     * src/demo.c
     */
    float nms = .4;
    layer l = net->layers[net->n-1];
    float *prediction = network_predict(net, boxed_image.data);

    memcpy(predictions[index], prediction, l.outputs*sizeof(float));
    mean_arrays(predictions, avg_frames, l.outputs, avg);
    l.output = avg;
    if (l.type == DETECTION) {
        get_detection_boxes(l, 1, 1, thresh, probs, boxes, 0);
    } else if (l.type == REGION) {
        get_region_boxes(l, img->w, img->h, net->w, net->h, thresh, probs, boxes, 0, 0, 0, hier, 1);
    } else {
        error("Last layer must produce detections\n");
        return -1;
    }
    if (nms > 0)
        do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);
    index = (index + 1) % avg_frames;

    /* Provide notification of detected objects to the invoking framework */
    log_detections(img->w, img->h, detections, thresh, boxes, probs, names, classes);

    return 0;
}
