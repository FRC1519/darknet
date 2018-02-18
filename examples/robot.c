#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

#include "robot.h"
#include "darknet.h"

#include "box.h"        // TODO Remove need
#include "image.h"      // TODO Remove need

/* Default configuration, overridable on the command line */
int opt_replay = 0;
int camera_port = 0;
char *stream_dest_host = "192.168.0.2";
int stream_dest_port = 1519;
int cap_width = 640;
int cap_height = 480;
char *cap_fps = "30/1";
char *video_filename = "/home/nvidia/capture.avi";
int bitrate = 1000000;
int iframe_ms = 2500;
int stream_width = 320;
int stream_height = 240;
char *stream_fps = "30/1";
float thresh = .24; /* Threshold to be exceeded to be considered worth reporting */

/* Synchronized access to image feed */
int next_frame = 0;
image next_img = { 0 };
int image_pending = 0;
pthread_mutex_t image_lock  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t image_cv = PTHREAD_COND_INITIALIZER;

/* Global indicator that it's time to be done */
int done = 0;

/* Command line options */
void parse_options(int argc, char **argv) {
    struct option long_opts[] = {
        {"gpu",           required_argument, NULL, 'i'},
        {"thresh",        required_argument, NULL, 't'},
        {"width",         required_argument, NULL, 'W'},
        {"height",        required_argument, NULL, 'H'},
        {"ip",            required_argument, NULL, 'I'},
        {"port",          required_argument, NULL, 'p'},
        {"fps",           required_argument, NULL, 'f'},
        {"video",         required_argument, NULL, 'V'},
        {"camera",        required_argument, NULL, 'c'},
        {"bitrate",       required_argument, NULL, 'b'},
        {"stream-width",  required_argument, NULL, 's'},
        {"stream-height", required_argument, NULL, 'S'},
        {"stream-fps",    required_argument, NULL, 'F'},
        {"replay",        no_argument,       NULL, 'R'},
        {NULL, 0, NULL, 0}
    };

    int long_index = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "h:i:t:a:W:H:I:p:f:V:c:b:s:S:F:R", long_opts, &long_index)) != -1) {
        switch (opt) {
            case 'i': gpu_index = atoi(optarg); break;
            case 't': thresh = atof(optarg); break;
            case 'W': cap_width = atoi(optarg); break;
            case 'H': cap_height = atoi(optarg); break;
            case 'I': stream_dest_host = optarg; break;
            case 'p': stream_dest_port = atoi(optarg); break;
            case 'f': cap_fps = optarg; break;
            case 'V': video_filename = optarg; break;
            case 'c': camera_port = atoi(optarg); break;
            case 'b': bitrate = atoi(optarg); break;
            case 's': stream_width = atoi(optarg); break;
            case 'S': stream_height = atoi(optarg); break;
            case 'F': stream_fps = optarg; break;
            case 'R': opt_replay = 1; break;
            default:
                error("usage error");
        }
    }
}

/* Detected objects in frames as they are found */
void *detect_thread_impl(void *ptr) {
    int last_frame = 0;
    int frame_delta;
    int rv;
    image img = { 0 };

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

        if (net_process_image(img, thresh) != 0) {
            fprintf(stderr, "Error from recognition network when processing image\n");
            done = 1;
        }
    }

    return NULL;
}

/* Fetch all frames from OpenCV */
void fetch_frames(CvCapture *cap) {
    int frame = 0;
    int status, rv;
    image new_image = { 0 };

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

        printf("Saving frame #%d for processing\n", frame);

        /* Ensure exclusive access */
        rv = pthread_mutex_lock(&image_lock);
        assert(rv == 0);

        /* Update image */
        if (next_img.data == NULL)
            next_img = copy_image(new_image);
        else
            copy_image_into(new_image, next_img);
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

    /* Pass along remaining arguments to recognition network */
    if (net_parse_arguments(argc - optind, &argv[optind]) != 0)
        error("Object recognition network failed to process arguments");

#ifdef GPU
    /* Initialize GPU */
    if (gpu_index >= 0){
        printf("Initializing CPU...\n");
        cuda_set_device(gpu_index);
    }
#endif

    /* Build up GStreamer pipepline */
    char gstreamer_cmd[512] = { '\0' };
    if (opt_replay)
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "filesrc location=%s ! avidemux ! tee name=t ! queue ! rtpjpegpay ! udpsink host=%s port=%d t. ! jpegdec ! videoconvert ! appsink", video_filename, stream_dest_host, stream_dest_port);
    else
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "uvch264src dev=/dev/video%d entropy=cabac post-previews=false rate-control=vbr initial-bitrate=%d peak-bitrate=%d average-bitrate=%d iframe-period=%d auto-start=true name=src src.vfsrc ! queue ! tee name=t ! queue ! image/jpeg, width=%d, height=%d, framerate=%s ! avimux ! filesink location=%s t. ! jpegdec ! videoconvert ! appsink src.vidsrc ! queue ! video/x-h264, width=%d, height=%d, framerate=%s, profile=high, stream-format=byte-stream ! h264parse ! video/x-h264, stream-format=avc ! rtph264pay ! udpsink host=%s port=%d", camera_port, bitrate, bitrate, bitrate, iframe_ms, cap_width, cap_height, cap_fps, video_filename, stream_width, stream_height, stream_fps, stream_dest_host, stream_dest_port);

    /* Use GStreamer to acquire video feed */
    printf("Connecting to GStreamer (%s)...\n", gstreamer_cmd);
    CvCapture *cap = cvCreateFileCaptureWithPreference(gstreamer_cmd, CV_CAP_GSTREAMER);
    if (!cap)
        error("Couldn't connect to GStreamer\n");
    printf("Connected to GStreamer\n");

    /* Kick off the Darknet magic */
    printf("Preparing network...\n");
    net_prepare();

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
