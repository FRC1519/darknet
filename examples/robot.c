#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

#include "darknet.h"

#include "network.h"
#include "detection_layer.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "image.h"
#include "demo.h"

/*
 * This code is based on the code that runs "darknet detector demo".
 * It is a trimmed down and edited version of darknet.c, detector.c, and demo.c
 */

/* Default configuration, overridable on the command line */
int opt_replay = 0;
int camera_port = 0;
char *stream_dest_host = "192.168.0.2";
int stream_dest_port = 1519;
int cap_width = 640;
int cap_height = 480;
int cap_fps = 30;
char *video_filename = "/home/nvidia/capture.avi";

float thresh = .24;
float hier = .5;
int avg_frames = 3;
int w = 0; /* TODO Compare to cap_width */
int h = 0; /* TODO Compare to cap_height */

float **probs;
box *boxes;
network *net;
image buff [3];
image buff_letter[3]; /* TODO Explore necessity of letter-boxing */
int buff_index = 0;

int detections = 0;
float **predictions;
int demo_index = 0;
float *avg;
int done = 0;
char **names;
int classes;

/* Thread synchronization */
pthread_barrier_t start_barrier, done_barrier;

void parse_options(int argc, char **argv) {
    struct option long_opts[] = {
        {"hier",   required_argument, NULL, 'h'},
        {"gpu",    required_argument, NULL, 'i'},
        {"thresh", required_argument, NULL, 't'},
        {"avg",    required_argument, NULL, 'a'},
        {"width",  required_argument, NULL, 'W'},
        {"height", required_argument, NULL, 'H'},
        {"ip",     required_argument, NULL, 'I'},
        {"port",   required_argument, NULL, 'p'},
        {"fps",    required_argument, NULL, 'f'},
        {"video",  required_argument, NULL, 'V'},
        {"camera", required_argument, NULL, 'c'},
        {"replay", no_argument,       NULL, 'R'},
        {NULL, 0, NULL, 0}
    };

    int long_index = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "h:i:t:a:W:H:I:p:f:V:c:R", long_opts, &long_index)) != -1) {
        switch (opt) {
            case 'h': hier = atof(optarg); break;
            case 'i': gpu_index = atoi(optarg); break;
            case 't': thresh = atof(optarg); break;
            case 'a': avg_frames = atoi(optarg); break;
            case 'W': w = atoi(optarg); break;
            case 'H': h = atoi(optarg); break;
            case 'I': stream_dest_host = optarg; break;
            case 'p': stream_dest_port = atoi(optarg); break;
            case 'f': cap_fps = atoi(optarg); break;
            case 'V': video_filename = optarg; break;
            case 'c': camera_port = atoi(optarg); break;
            case 'R': opt_replay = 1; break;
            default:
                error("usage error");
        }
    }
}

void log_detections(image im, int num, float thresh, box *boxes, float **probs, char **names, int classes)
{
    int i,j;

    for(i = 0; i < num; ++i){
        char labelstr[4096] = {0};
        int class = -1;

        // TODO Change to finding the MOST probable, instead of finding them all, or just the first
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

/*
 * Prepare the network for processing
 * Code code from demo() in src/demo.c
 */
void prepare_network(CvCapture *cap, char *cfgfile, char *weightfile) {
    /* Load in network from config file and weights */
    predictions = calloc(avg_frames, sizeof(float*));
    net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);

    layer l = net->layers[net->n-1];
    detections = l.n*l.w*l.h;
    int j;

    avg = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < avg_frames; ++j) predictions[j] = (float *) calloc(l.outputs, sizeof(float));

    boxes = (box *)calloc(l.w*l.h*l.n, sizeof(box));
    probs = (float **)calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = (float *)calloc(l.classes+1, sizeof(float));

    buff[0] = get_image_from_stream(cap);
    buff[1] = copy_image(buff[0]);
    buff[2] = copy_image(buff[0]);
    buff_letter[0] = letterbox_image(buff[0], net->w, net->h);
    buff_letter[1] = letterbox_image(buff[0], net->w, net->h);
    buff_letter[2] = letterbox_image(buff[0], net->w, net->h);
}

void *detect_in_thread(void *ptr)
{
    while (!done) {
        pthread_barrier_wait(&start_barrier);

        float nms = .4;
        layer l = net->layers[net->n-1];
        float *X = buff_letter[(buff_index+2)%3].data;
        float *prediction = network_predict(net, X);

        memcpy(predictions[demo_index], prediction, l.outputs*sizeof(float));
        mean_arrays(predictions, avg_frames, l.outputs, avg);
        l.output = avg;
        if(l.type == DETECTION){
            get_detection_boxes(l, 1, 1, thresh, probs, boxes, 0);
        } else if (l.type == REGION){
            get_region_boxes(l, buff[0].w, buff[0].h, net->w, net->h, thresh, probs, boxes, 0, 0, 0, hier, 1);
        } else {
            done = 1;
            error("Last layer must produce detections\n");
        }
        if (nms > 0) do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);

        printf("Objects:\n\n");
        image display = buff[(buff_index+2) % 3];
        log_detections(display, detections, thresh, boxes, probs, names, classes);

        demo_index = (demo_index + 1) % avg_frames;

        pthread_barrier_wait(&done_barrier);
    }

    return NULL;
}

void *fetch_in_thread(void *ptr)
{
    CvCapture *cap = ptr;
    int status;

    while (!done) {
        pthread_barrier_wait(&start_barrier);

        status = fill_image_from_stream(cap, buff[buff_index]);
        if (status == 0) {
            done = 1;
        } else {
            letterbox_image_into(buff[buff_index], net->w, net->h, buff_letter[buff_index]);
        }

        pthread_barrier_wait(&done_barrier);
    }

    return NULL;
}

int main(int argc, char **argv)
{
#ifndef GPU
    gpu_index = -1;
#endif

    /* Process command-line options */
    parse_options(argc, argv);

    /* Process data file parameters */
    if (argc != optind + 3) {
        fprintf(stderr, "usage: %s [datacfg] [cfg] [weights]\n", argv[0]);
        exit(1);
    }
    char *datacfg = argv[optind++];
    char *cfgfile = argv[optind++];
    char *weightfile = argv[optind++];

#ifdef GPU
    /* Initialize GPU */
    if (gpu_index >= 0){
        printf("Initializing CPU\n");
        cuda_set_device(gpu_index);
    }
#endif

    /* Read in data config */
    list *options = read_data_cfg(datacfg);
    classes = option_find_int(options, "classes", 20);
    char *name_list = option_find_str(options, "names", NULL);
    if (!name_list) {
        error("Name list not defined in data configuration\n");
    }
    names = get_labels(name_list);

    /* Build up GStreamer pipepline */
    char gstreamer_cmd[512] = { '\0' };
    if (opt_replay)
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "filesrc location=%s ! avidemux ! tee name=t ! queue ! rtpjpegpay ! udpsink host=%s port=%d t. ! jpegdec ! videoconvert ! appsink", video_filename, stream_dest_host, stream_dest_port);
    else
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "v4l2src device=/dev/video%d ! image/jpeg, width=%d, height=%d, framerate=%d ! tee name=t ! queue ! avimux ! filesink location=%s t. ! queue ! rtpjpegpay ! udpsink host=%s port=%d t. ! jpegdec ! videoconvert ! appsink", camera_port, cap_width, cap_height, cap_fps, video_filename, stream_dest_host, stream_dest_port);

    /* Use GStreamer to acquire video feed */
    printf("Connecting to GStreamer (%s)...\n", gstreamer_cmd);
    CvCapture *cap = cvCreateFileCaptureWithPreference(gstreamer_cmd, CV_CAP_GSTREAMER);
    if (!cap)
        error("Couldn't connect to GStreamer\n");
    printf("Connected to GStreamer\n");

    printf("Preparing network...\n");
    prepare_network(cap, cfgfile, weightfile);

    int count = 1;
    pthread_t detect_thread, fetch_thread;
    if (pthread_create(&fetch_thread, NULL, fetch_in_thread, cap))
        error("Thread creation failed");
    if (pthread_create(&detect_thread, NULL, detect_in_thread, NULL))
        error("Thread creation failed");
    pthread_barrier_init(&start_barrier, NULL, 3);
    pthread_barrier_init(&done_barrier, NULL, 3);

    /* TODO Allow threads to run independently, using better synchronization (like a counting semaphore) */

    while (!done) {
        printf("\nFRAME #%d\n", count);
        buff_index = (buff_index + 1) % 3;

        pthread_barrier_wait(&start_barrier);
        pthread_barrier_wait(&done_barrier);

        ++count;
    }

    pthread_barrier_destroy(&start_barrier);
    pthread_barrier_destroy(&done_barrier);
    pthread_join(fetch_thread, 0);
    pthread_join(detect_thread, 0);
}
