#include "darknet.h"

#include <stdlib.h>
#include <stdio.h>

#define VIDEO_FILE "/home/nvidia/capture.avi"
#ifdef GPU
#define CAM_CAP "image/jpeg, width=640, height=480, framerate=30/1"
#else
#define CAM_CAP "image/jpeg, width=320, height=240, framerate=15/1"
#endif
#define CAM_DEV "/dev/video0"
#define STREAM_DEST_HOST "192.168.0.73"
#define STREAM_DEST_PORT "9999"

#define GSTREAMER_COMMAND "filesrc location=" VIDEO_FILE " ! avidemux ! tee name=t ! queue ! rtpjpegpay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! jpegdec ! videoconvert ! appsink"
//#define GSTREAMER_COMMAND "v4l2src device=" CAM_DEV " ! " CAM_CAP " ! tee name=t ! queue ! avimux ! filesink location=" VIDEO_FILE " t. ! queue ! rtpjpegpay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! jpegdec ! videoconvert ! appsink"

void robot_demo(char *cfgfile, char *weightfile, float thresh, char **names, int classes, int avg_frames, float hier, int w, int h);

/*
 * This code is based on the code that runs "darknet detector demo".
 * It is a trimmed down and edited version of darknet.c, detector.c, and demo.c
 */

int main(int argc, char **argv)
{

/* TODO What do do with this GPU detection? */
    gpu_index = find_int_arg(argc, argv, "-i", 0);
    if(find_arg(argc, argv, "-nogpu")) {
        gpu_index = -1;
    }

#ifndef GPU
    gpu_index = -1;
#else
    if(gpu_index >= 0){
        cuda_set_device(gpu_index);
    }
#endif

    float thresh = find_float_arg(argc, argv, "-thresh", .24);
    float hier_thresh = find_float_arg(argc, argv, "-hier", .5);
    int avg = find_int_arg(argc, argv, "-avg", 3);
    if(argc < 4){
        fprintf(stderr, "usage: %s %s [cfg] [weights (optional)]\n", argv[0], argv[1]);
        return 0;
    }
    char *gpu_list = find_char_arg(argc, argv, "-gpus", 0);
    int *gpus = 0;
    int gpu = 0;
    int ngpus = 0;
    if(gpu_list){
        printf("%s\n", gpu_list);
        int len = strlen(gpu_list);
        ngpus = 1;
        int i;
        for(i = 0; i < len; ++i){
            if (gpu_list[i] == ',') ++ngpus;
        }
        gpus = calloc(ngpus, sizeof(int));
        for(i = 0; i < ngpus; ++i){
            gpus[i] = atoi(gpu_list);
            gpu_list = strchr(gpu_list, ',')+1;
        }
    } else {
        gpu = gpu_index;
        gpus = &gpu;
        ngpus = 1;
    }

    int width = find_int_arg(argc, argv, "-w", 0);
    int height = find_int_arg(argc, argv, "-h", 0);

    char *datacfg = argv[1];
    char *cfg = argv[2];
    char *weights = (argc > 3) ? argv[3] : 0;

    list *options = read_data_cfg(datacfg);
    int classes = option_find_int(options, "classes", 20);
    char *name_list = option_find_str(options, "names", "data/names.list");
    char **names = get_labels(name_list);

    robot_demo(cfg, weights, thresh, names, classes, avg, hier_thresh, width, height);
}

#include "network.h"
#include "detection_layer.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "image.h"
#include "demo.h"

static char **demo_names;
static int demo_classes;

static float **probs;
static box *boxes;
static network *net;
static image buff [3];
static image buff_letter[3];
static int buff_index = 0;
static CvCapture * cap;
static float demo_thresh = 0;
static float demo_hier = .5;
static int running = 0;

static int demo_frame = 3;
static int demo_detections = 0;
static float **predictions;
static int demo_index = 0;
static float *avg;
static int demo_done = 0;

void log_detections(image im, int num, float thresh, box *boxes, float **probs, char **names, int classes)
{
    int i,j;

    for(i = 0; i < num; ++i){
        char labelstr[4096] = {0};
        int class = -1;

        // TODO Change to finding the MOST probably, instead of finding them all, or just the first
        for(j = 0; j < classes; ++j){
            if (probs[i][j] > thresh){
                if (class < 0) {
                    strcat(labelstr, names[j]);
                    class = j;
                } else {
                    strcat(labelstr, ", ");
                    strcat(labelstr, names[j]);
                }
                printf("%s: %.0f%%\n", names[j], probs[i][j]*100);
            }
        }
        if(class >= 0){
            //printf("%d %s: %.0f%%\n", i, names[class], prob*100);

            box b = boxes[i];

            int left  = (b.x-b.w/2.)*im.w;
            int right = (b.x+b.w/2.)*im.w;
            int top   = (b.y-b.h/2.)*im.h;
            int bot   = (b.y+b.h/2.)*im.h;

            if(left < 0) left = 0;
            if(right > im.w-1) right = im.w-1;
            if(top < 0) top = 0;
            if(bot > im.h-1) bot = im.h-1;
        }
    }
}

void *detect_in_thread(void *ptr)
{
    running = 1;
    float nms = .4;

    layer l = net->layers[net->n-1];
    float *X = buff_letter[(buff_index+2)%3].data;
    float *prediction = network_predict(net, X);

    memcpy(predictions[demo_index], prediction, l.outputs*sizeof(float));
    mean_arrays(predictions, demo_frame, l.outputs, avg);
    l.output = avg;
    if(l.type == DETECTION){
        get_detection_boxes(l, 1, 1, demo_thresh, probs, boxes, 0);
    } else if (l.type == REGION){
        get_region_boxes(l, buff[0].w, buff[0].h, net->w, net->h, demo_thresh, probs, boxes, 0, 0, 0, demo_hier, 1);
    } else {
        error("Last layer must produce detections\n");
    }
    if (nms > 0) do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);

    printf("Objects:\n\n");
    image display = buff[(buff_index+2) % 3];
    log_detections(display, demo_detections, demo_thresh, boxes, probs, demo_names, demo_classes);

    demo_index = (demo_index + 1)%demo_frame;
    running = 0;
    return 0;
}

void *fetch_in_thread(void *ptr)
{
    int status = fill_image_from_stream(cap, buff[buff_index]);
    letterbox_image_into(buff[buff_index], net->w, net->h, buff_letter[buff_index]);
    if(status == 0) demo_done = 1;
    return 0;
}

void robot_demo(char *cfgfile, char *weightfile, float thresh, char **names, int classes, int avg_frames, float hier, int w, int h)
{
    demo_frame = avg_frames;
    predictions = calloc(demo_frame, sizeof(float*));
    demo_names = names;
    demo_classes = classes;
    demo_thresh = thresh;
    demo_hier = hier;
    net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);
    pthread_t detect_thread;
    pthread_t fetch_thread;

    printf("Connecting to GStreamer (%s)\n", GSTREAMER_COMMAND);
    cap = cvCreateFileCaptureWithPreference(GSTREAMER_COMMAND, CV_CAP_GSTREAMER);
    if (!cap) error("Couldn't connect to GStreamer.\n");
    printf("Connected to GStreamer.\n");

    layer l = net->layers[net->n-1];
    demo_detections = l.n*l.w*l.h;
    int j;

    avg = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < demo_frame; ++j) predictions[j] = (float *) calloc(l.outputs, sizeof(float));

    boxes = (box *)calloc(l.w*l.h*l.n, sizeof(box));
    probs = (float **)calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = (float *)calloc(l.classes+1, sizeof(float));

    buff[0] = get_image_from_stream(cap);
    buff[1] = copy_image(buff[0]);
    buff[2] = copy_image(buff[0]);
    buff_letter[0] = letterbox_image(buff[0], net->w, net->h);
    buff_letter[1] = letterbox_image(buff[0], net->w, net->h);
    buff_letter[2] = letterbox_image(buff[0], net->w, net->h);

    int count = 1;

    while (!demo_done) {
        printf("\nFRAME #%d\n", count);
        buff_index = (buff_index + 1) % 3;

        /* TODO Notify long-running threads, instead of constantly creating and destroying threads */
        if(pthread_create(&fetch_thread, 0, fetch_in_thread, 0)) error("Thread creation failed");
        if(pthread_create(&detect_thread, 0, detect_in_thread, 0)) error("Thread creation failed");

        pthread_join(fetch_thread, 0);
        pthread_join(detect_thread, 0);
        ++count;
    }
}
