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
void net_prepare(int gpu_idx) {
    /* Load in network from config file and weights */
    predictions = calloc(avg_frames, sizeof(float*));
    net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);

    gpu_index = gpu_idx;
#ifdef GPU
    /* Initialize GPU */
    if (gpu_index >= 0){
        printf("Initializing CPU...\n");
        cuda_set_device(gpu_index);
    }
#endif

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
        images[j] = make_empty_image(0, 0, 0);
        free_images[j] = &images[j];
    }
}

/* Process the detected objects */
void process_detections(object_location *obj, int w, int h, int num, float thresh, box *boxes, float **probs, char **names, int classes) {
    /* This function is adapated from draw_detections() in src/image.c */
    int i, j;

    /* Initialize the list of detected objects */
    for (j = 0; j < MAX_OBJECTS_PER_FRAME; j++)
        obj[j].type = OBJ_NONE;

    printf("Objects:\n"); // TODO Remove after debugging

    /* Find the most probable object at this location */
    for (i = 0; i < num; ++i) {
        int class = -1;
        float prob = 0.0;

        /*
         * Search through all possible classes, to see if any are above the
         * given threshold
         */
        for (j = 0; j < classes; ++j) {
            if (probs[i][j] > thresh) {
                printf("  %s: %.0f%%\n", names[j], probs[i][j]*100); // TODO Remove after debugging

                /*
                 * Check if this object is more likely that any other objects at
                 * this location -- we only want to track one object at any
                 * given location
                 */
                if (probs[i][j] > prob) {
                    class = j;
                    prob = probs[i][j];
                } else {
                    printf("    --> not better than %s @ %.0f%%\n", names[class], prob); // TODO Remove after debugging
                }
            }
        }

        /* Process the object, if one was found */
        if (class >= 0){
            box b = boxes[i];
            object_location tmp_obj, tmp2_obj;

            printf("  %d %s: %.0f%%\n", i, names[class], prob*100); // TODO Remove after debugging

            /*
             * Find location in the list for the object -- it is kept in
             * descending order of proabability
             */
            for (j = 0; j < MAX_OBJECTS_PER_FRAME; j++) {
                if (obj[j].type == OBJ_NONE || prob > obj[j].probability) {
                    tmp_obj = obj[j];
                    break;
                }
            }

            /* Add the object to the list */
            if (j < MAX_OBJECTS_PER_FRAME) {
                obj[j].type = class + 1; /* Classes start at 0, objects at 1 */
                obj[j].x = b.x;
                obj[j].y = b.y;
                obj[j].width = b.w;
                obj[j].height = b.h;
                obj[j].probability = prob;
            }

            /* Shuffle any lower probability objects down the list */
            for (j++; j < MAX_OBJECTS_PER_FRAME; j++) {
                tmp2_obj = obj[j];
                obj[j] = tmp_obj;

                /* Check if the last object has been found */
                if (tmp2_obj.type == OBJ_NONE)
                    break;
            }
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
            break;
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
int net_process_image(void *image_data, float thresh, object_location *objects) {
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
    process_detections(objects, img->w, img->h, detections, thresh, boxes, probs, names, classes);

    return 0;
}
