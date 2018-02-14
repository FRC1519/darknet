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
char *cap_fps = "30/1";
char *video_filename = "/home/nvidia/capture.avi";

/* Network-related configuration, overridable on the command line */
float thresh = .24;
float hier = .5;
int avg_frames = 3;

/* Synchronized access to image feed */
int next_frame = 0;
image next_img = { 0 };
int image_pending = 0;
pthread_mutex_t image_lock  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t image_cv = PTHREAD_COND_INITIALIZER;

/* Global indicator that it's time to be done */
int done = 0;

/* "Magic" darknet stuff */
network *net;
float **probs;
box *boxes;
int detections = 0;
float **predictions;
float *avg;
char **names;
int classes;

/* Command line options */
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
            case 'W': cap_width = atoi(optarg); break;
            case 'H': cap_height = atoi(optarg); break;
            case 'I': stream_dest_host = optarg; break;
            case 'p': stream_dest_port = atoi(optarg); break;
            case 'f': cap_fps = optarg; break;
            case 'V': video_filename = optarg; break;
            case 'c': camera_port = atoi(optarg); break;
            case 'R': opt_replay = 1; break;
            default:
                error("usage error");
        }
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

            /* TODO Implement real notification method */
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
 * Prepare the darknet network for processing
 * Code derived from demo() in src/demo.c
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
}

/* Detected objects in frames as they are found */
void *detect_thread_impl(void *ptr) {
    int last_frame = 0;
    int frame_delta;
    int rv;
    image img = { 0 };
    int index = 0;

    /* Run until the program is ready to be over */
    while (!done) {
        printf("Waiting on a frame to process...\n");

        /* Ensure exclusive access */
        rv = pthread_mutex_lock(&image_lock);
        assert(rv == 0);

        /* Wait for a new frame */
        while (!image_pending) {
            /* Wait for notification that something has been added */
            rv = pthread_cond_wait(&image_cv, &image_lock);
            assert(rv == 0);
        }

        /* Check the status of new frames */
        assert(next_frame >= last_frame);
        frame_delta = next_frame - last_frame;
        if (frame_delta == 0) {
            pthread_mutex_unlock(&image_lock);
            printf("Detector woke up without a new frame (%d), expected %d frame(s)\n", last_frame, image_pending);
            continue;
        }
        if (frame_delta != image_pending)
            printf("NOTE: Expected to find %d new frame(s), but found %d instead\n", image_pending, frame_delta);

        /* Get the data from the image */
        if (img.data == NULL)
            img = copy_image(next_img);
        else
            copy_image_into(next_img, img);
        last_frame = next_frame;
        image_pending = 0;

        /* Remove exclusive access */
        pthread_mutex_unlock(&image_lock);

        /*
         * Checked for missed frame -- it happens if we cannot detect as quickly
         * as new frames arrive, but we'd like to know about it
         */
        if (frame_delta > 1)
            printf("NOTE: Detector missed %d frame(s)\n", frame_delta - 1);

        /* "Magic" Darknet stuff */
        float nms = .4;
        layer l = net->layers[net->n-1];
        float *prediction = network_predict(net, img.data);

        memcpy(predictions[index], prediction, l.outputs*sizeof(float));
        mean_arrays(predictions, avg_frames, l.outputs, avg);
        l.output = avg;
        if(l.type == DETECTION){
            get_detection_boxes(l, 1, 1, thresh, probs, boxes, 0);
        } else if (l.type == REGION){
            get_region_boxes(l, cap_width, cap_height, net->w, net->h, thresh, probs, boxes, 0, 0, 0, hier, 1);
        } else {
            done = 1;
            error("Last layer must produce detections\n");
        }
        if (nms > 0) do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);
        index = (index + 1) % avg_frames;

        log_detections(cap_width, cap_height, detections, thresh, boxes, probs, names, classes);
    }

    return NULL;
}

/* Fetch all frames from OpenCV */
void fetch_frames(CvCapture *cap) {
    int frame = 0;
    int status, rv;
    image new_image = { 0 };
    image boxed_image = { 0 };

    while (!done) {
        printf("Acquiring frame #%d\n", ++frame);

        if (new_image.data == NULL) {
            new_image = get_image_from_stream(cap);
        } else {
            status = fill_image_from_stream(cap, new_image);
            if (status == 0) {
                done = 1;
                rv = pthread_cond_signal(&image_cv);
                assert(rv == 0);
                return;
            }
        }
        assert(new_image.w == cap_width);
        assert(new_image.h == cap_height);

        if (new_image.w == net->w && new_image.h == net->h)
            boxed_image = new_image;
        else if (boxed_image.data == NULL)
            boxed_image = letterbox_image(new_image, net->w, net->h);
        else
            letterbox_image_into(new_image, net->w, net->h, boxed_image);

        printf("Saving frame #%d for processing\n", frame);

        /* Ensure exclusive access */
        rv = pthread_mutex_lock(&image_lock);
        assert(rv == 0);

        /* Update image */
        if (next_img.data == NULL)
            next_img = copy_image(boxed_image);
        else
            copy_image_into(boxed_image, next_img);
        next_frame = frame;
        image_pending++;

        /* Release exclusivity ASAP */
        rv = pthread_mutex_unlock(&image_lock);
        assert(rv == 0);

        /* Notify detector that there might be a new frame */
        rv = pthread_cond_signal(&image_cv);
        assert(rv == 0);
    }
}

/* Main program */
int main(int argc, char **argv) {
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
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "v4l2src device=/dev/video%d ! image/jpeg, width=%d, height=%d, framerate=%s ! tee name=t ! queue ! avimux ! filesink location=%s t. ! queue ! rtpjpegpay ! udpsink host=%s port=%d t. ! jpegdec ! videoconvert ! appsink", camera_port, cap_width, cap_height, cap_fps, video_filename, stream_dest_host, stream_dest_port);

    /* Use GStreamer to acquire video feed */
    printf("Connecting to GStreamer (%s)...\n", gstreamer_cmd);
    CvCapture *cap = cvCreateFileCaptureWithPreference(gstreamer_cmd, CV_CAP_GSTREAMER);
    if (!cap)
        error("Couldn't connect to GStreamer\n");
    printf("Connected to GStreamer\n");

    /* Kick off the Darknet magic */
    printf("Preparing network...\n");
    prepare_network(cap, cfgfile, weightfile);

    /* Start detector asynchronously */
    pthread_t detect_thread;
    printf("Starting detector thread...\n");
    if (pthread_create(&detect_thread, NULL, detect_thread_impl, NULL))
        error("Thread creation failed");

    /* Process all of the frames */
    printf("Fetching frames from source...\n");
    fetch_frames(cap);

    /* Clean up */
    printf("Waiting for termination...\n");
    pthread_join(detect_thread, 0);
    cvReleaseCapture(&cap);

    return 0;
}
