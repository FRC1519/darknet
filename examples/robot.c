/*
 * TODO Consider a C++ implementation now that it's cleanly separable from the
 *      object detection AI network implementation, since C++ is better
 *      supported by both OpenCV and NetworkTables
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <inttypes.h>
#include <assert.h>
#include <sys/time.h>
#include <endian.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <opencv2/core/core_c.h>
#include <opencv2/videoio/videoio_c.h>
#include <ntcore.h>

#include "robot.h"

/* Default configuration, overridable on the command line */
int opt_gpu = -1;
int opt_replay = 0;
int opt_broadcast = 0;
int camera_dev = 0;
int camera2_dev = -1;
char *robot_host = "10.15.19.2";
char *oper_host = "10.15.19.5";
int video_port = 1190;
int data_port = 5810;
int cam_port = 1519;
int cam2_port = 1520;
int cap_width = 640;
int cap_height = 480;
char *cap_fps = "30/1";
char *video_filename = "/home/nvidia/capture.avi";
int bitrate = 1000000;
int iframe_ms = 2500;
int stream_width = 432;
int stream_height = 240;
char *stream_fps = "30/1";
char *data_log_fn = NULL;
FILE *data_log_fp = NULL;
float thresh = .14; /* Threshold to be exceeded to be considered worth reporting */
volatile int active_cam = 0;

/* Synchronized access to image feed */
void *next_image_data = NULL;
int next_frame = 0;
pthread_mutex_t image_lock  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t image_cv = PTHREAD_COND_INITIALIZER;

/* Global indicator that it's time to be done */
int done = 0;

/* Globals used for IP network broadcasts */
struct sockaddr_in robot_addr, oper_addr;
int sock;

/* Command line options */
void parse_options(int argc, char **argv) {
    struct option long_opts[] = {
        {"gpu",           required_argument, NULL, 'i'},
        {"thresh",        required_argument, NULL, 't'},
        {"width",         required_argument, NULL, 'W'},
        {"height",        required_argument, NULL, 'H'},
        {"robot-ip",      required_argument, NULL, 'r'},
        {"oper-ip",       required_argument, NULL, 'o'},
        {"video-port",    required_argument, NULL, 'p'},
        {"data-port",     required_argument, NULL, 'P'},
        {"fps",           required_argument, NULL, 'f'},
        {"video",         required_argument, NULL, 'V'},
        {"camera",        required_argument, NULL, 'c'},
        {"camera2",       required_argument, NULL, 'C'},
        {"bitrate",       required_argument, NULL, 'b'},
        {"stream-width",  required_argument, NULL, 's'},
        {"stream-height", required_argument, NULL, 'S'},
        {"stream-fps",    required_argument, NULL, 'F'},
        {"data-log",      required_argument, NULL, 'l'},
        {"replay",        no_argument,       NULL, 'R'},
        {"broadcast",     no_argument,       NULL, 'B'},
        {"iframe",        required_argument, NULL, 'I'},
        {NULL, 0, NULL, 0}
    };

    int long_index = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "h:i:t:a:W:H:r:o:p:P:f:V:c:C:b:s:S:F:l:RBI:", long_opts, &long_index)) != -1) {
        switch (opt) {
            case 'i': opt_gpu = atoi(optarg); break;
            case 't': thresh = atof(optarg); break;
            case 'W': cap_width = atoi(optarg); break;
            case 'H': cap_height = atoi(optarg); break;
            case 'r': robot_host = optarg; break;
            case 'o': oper_host = optarg; break;
            case 'p': video_port = atoi(optarg); break;
            case 'P': data_port = atoi(optarg); break;
            case 'f': cap_fps = optarg; break;
            case 'V': video_filename = optarg; break;
            case 'c': camera_dev = atoi(optarg); break;
            case 'C': camera2_dev = atoi(optarg); break;
            case 'b': bitrate = atoi(optarg); break;
            case 's': stream_width = atoi(optarg); break;
            case 'S': stream_height = atoi(optarg); break;
            case 'F': stream_fps = optarg; break;
            case 'l': data_log_fn = optarg; break;
            case 'R': opt_replay = 1; break;
            case 'B': opt_broadcast = 1; break;
            case 'I': iframe_ms = atoi(optarg); break;
            default:
                fprintf(stderr, "Usage error\n");
                exit(1);
        }
    }
}

void host_to_addr(char *host, struct sockaddr_in *sin) {
    struct hostent *hent;

    /* Look up the IP address */
    hent = gethostbyname(host);
    if (hent == NULL) {
        fprintf(stderr, "Unknown host \"%s\"\n", host);
        exit(1);
    }
    
    /* Initialize address */
    sin->sin_family = hent->h_addrtype;
    memcpy(&sin->sin_addr.s_addr, hent->h_addr_list[0], hent->h_length);
    sin->sin_port = htons(data_port);
}

/* Initialize the IP network */
void ip_network_init(void) {
    int rv;

    /* Convert host names to addresses */
    host_to_addr(robot_host, &robot_addr);
    host_to_addr(oper_host, &oper_addr);

    /* Create datagram socket for communication */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to open socket\n");
        exit(1);
    }

    /* Set up the local address flexibly */
    struct sockaddr_in our_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(0),
    };

    /* Bind the socket locally */
    rv = bind(sock, (struct sockaddr *)&our_addr, sizeof(our_addr));
    if (rv < 0) {
        fprintf(stderr, "Cannot bind to local port\n");
        exit(1);
    }
}

/* Provide notification about detected objects */
void notify_objects(object_location *objects, int frame) {
    datagram data = { 0 };
    struct timeval tv;
    uint64_t timestamp;
    int rv;
    int i;
    
    rv = gettimeofday(&tv, NULL);
    if (rv < 0) {
        fprintf(stderr, "Failed to get time of day\n");
        exit(1);
    }

    data.magic = htobe32(MAYHEM_MAGIC);
    data.frame_number = htobe32(frame);
    timestamp = tv.tv_sec * 1000000ULL + tv.tv_usec;
    data.timestamp = htobe64(timestamp);

    for (i = 0; i < MAX_OBJECTS_PER_FRAME; i++) {
        /* Quit early if no more objects */
        if (objects[i].type == OBJ_NONE)
            break;

        data.object_data[i].type = htobe32(objects[i].type);
        data.object_data[i].x = htobe32(objects[i].x * INT32_MAX);
        data.object_data[i].y = htobe32(objects[i].y * INT32_MAX);
        data.object_data[i].width = htobe32(objects[i].width * INT32_MAX);
        data.object_data[i].height = htobe32(objects[i].height * INT32_MAX);
        data.object_data[i].probability = htobe32(objects[i].probability * INT32_MAX);

        if (data_log_fp != NULL)
            fprintf(data_log_fp, "%d,%" PRIu64 ",%d,%.4f,%.4f,%.4f,%.4f,%.4f\n", frame, timestamp, objects[i].type, objects[i].x, objects[i].y, objects[i].width, objects[i].height, objects[i].probability);
    }

    /* Output at least one line to record that no object were detected */
    if (i == 0 && data_log_fp != NULL)
        fprintf(data_log_fp, "%u,%" PRIu64 ",%d,%.4f,%.4f,%.4f,%.4f,%.4f\n", frame, timestamp, OBJ_NONE, 0.0, 0.0, 0.0, 0.0, 0.0);

    /* Send notification of objects to robot */
    rv = sendto(sock, &data, sizeof(data), 0, (struct sockaddr *)&robot_addr, sizeof(robot_addr));
    if (rv < 0)
        fprintf(stderr, "WARNING: Failed to send notification about objects in frame #%u to robot\n", frame);

    /* Send notification of objects to operator */
    rv = sendto(sock, &data, sizeof(data), 0, (struct sockaddr *)&oper_addr, sizeof(oper_addr));
    if (rv < 0)
        fprintf(stderr, "WARNING: Failed to send notification about objects in frame #%u to operator\n", frame);

    /* Ensure data is flushed to file */
    if (data_log_fp != NULL)
        fflush(data_log_fp);
}

/* Set the active camera */
void change_camera(int camera) {
    char cmd[255];
    int rv;

    /* Switch source of frames */
    active_cam = camera == 1 ? 1 : 0;

    /* Determine the port to block and the port to allow */
    int good_port, bad_port;
    if (active_cam == 0) {
        good_port = cam_port;
        bad_port = cam2_port;
    } else {
        good_port = cam2_port;
        bad_port = cam_port;
    }

    /*
     * NOTE: There is no good interface to iptables.  There's libiptc, but it's
     * not stable.  It's also very low level.  Therefore, just use system().
     * It's fast enough.
     */

    /* First, stop the camera stream that's not active */
    snprintf(cmd, sizeof(cmd), "/sbin/iptables -A OUTPUT -p udp --sport %d -j DROP", bad_port);
    rv = system(cmd);
    if (rv == 0)
        fprintf(stderr, "WARNING: Failed to limit traffic from port %d\n", bad_port);

    /* Second, allow the camera stream that's now active */
    snprintf(cmd, sizeof(cmd), "/sbin/iptables -D OUTPUT -p udp --sport %d -j DROP", good_port);
    rv = system(cmd);
    if (rv == 0)
        fprintf(stderr, "WARNING: Failed to limit traffic from port %d\n", bad_port);
}

/* Monitor Network Tables for changes to the active camera */
void *camera_monitor(void *ptr) {
#ifndef RANDOM_CAMERA_SWITCH
    NT_Inst inst = NT_GetDefaultInstance();

    const char *valname = NT_ENTRYNAME_CAMERA;

    /* Connect to robot */
    const char *myname = "Vision";
    NT_SetNetworkIdentity(inst, myname, strlen(myname));
    NT_StartClient(inst, robot_host, NT_DEFAULT_PORT);
    while (!NT_IsConnected(inst)) {
        printf("Waiting to connect to robot network tables server...\n");
        sleep(5);
    }
    printf("Connected to robot network tables server\n");

    /* Configure listener to receive updates from robot */
    NT_EntryListenerPoller poller = NT_CreateEntryListenerPoller(inst);
    NT_Entry entry = NT_GetEntry(inst, valname, strlen(valname));
    int poll_flags = NT_NOTIFY_IMMEDIATE | NT_NOTIFY_NEW | NT_NOTIFY_UPDATE;
    NT_AddPolledEntryListenerSingle(poller, entry, poll_flags);

    /* Handle the current value */
    uint64_t timestamp;
    double vDouble;
    int value;
    if (NT_GetEntryDouble(entry, &timestamp, &vDouble)) {
        value = (int)(vDouble + 0.5);
        printf("Network table original value at @%" PRIu64 "]: %d\n", timestamp, value);
        change_camera(value);
    } else {
        fprintf(stderr, "NOTE: Unable to get initial camera selection\n");
    }

    /* Monitor for changes */
    size_t len;
    double timeout = 10.0;
    NT_Bool timed_out;
    struct NT_EntryNotification *notification;
    while (!done) {
        printf("Polling for Network Table update...\n");
        notification = NT_PollEntryListenerTimeout(poller, &len, timeout, &timed_out);
        if (timed_out) {
            printf("No network tables update received before timeout\n");
            if (!NT_IsConnected(inst)) {
                fprintf(stderr, "Network tables are no longer connected to the robot\n");
                done = 1;
                return NULL;
            }
            continue;
        }

        /* Extract camera index from entry */
        if (!NT_GetValueDouble(&notification->value, &timestamp, &vDouble)) {
            fprintf(stderr, "Failed to get double value from network table entry\n");
            continue;
        }
        value = (int)(vDouble + 0.5);
        printf("Network table update at @%" PRIu64 "]: %d\n", timestamp, value);
        NT_DisposeEntryNotification(notification);

        /* Switch to the active camera */
        change_camera(value);
    }
    
    /* Clean up and exit */
    NT_DestroyEntryListenerPoller(poller);
    return NULL;
#else /* RANDOM_CAMERA_SWITCH */
    int value = 0;
    while (!done) {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 300000000 };
        printf("\n\n\n\t\tSLEEPING\n\n\n");
        nanosleep(&ts, NULL);
        value = (value + 1) % 2;
        printf("\n\n\n\t\tCHANGING CAM ====> %d <====\n\n\n");
        change_camera(value);
    }
    return NULL;
#endif /* RANDOM_CAMERA_SWITCH */
}

/* Detected objects in frames as they are found */
void *detect_thread_impl(void *ptr) {
    object_location objects[MAX_OBJECTS_PER_FRAME] = { 0 };
    int last_frame = 0;
    int frame_delta;
    int rv;
    void *net_data;

    /* Run until the program is ready to be over */
    while (!done) {
        printf("Waiting on a frame to process...\n");

        /* Ensure exclusive access */
        rv = pthread_mutex_lock(&image_lock);
        assert(rv == 0);

        /* Wait for a new frame */
        while (next_image_data == NULL) {
            /* Wait for notification that something has been added */
            rv = pthread_cond_wait(&image_cv, &image_lock);
            assert(rv == 0);
        }

        /* Check the status of new frames */
        assert(next_frame >= last_frame);
        frame_delta = next_frame - last_frame;
        if (frame_delta == 0) {
            pthread_mutex_unlock(&image_lock);
            printf("Detector woke up without a new frame (still %d)\n", last_frame);
            continue;
        }

        /* Get the data from the image */
        net_data = next_image_data;
        next_image_data = NULL;
        last_frame = next_frame;

        /* Remove exclusive access */
        pthread_mutex_unlock(&image_lock);

        /*
         * Checked for missed frame -- it happens if we cannot detect as quickly
         * as new frames arrive, and we'd like to know about it
         */
        if (frame_delta > 1)
            printf("NOTE: Detector missed %d frame(s)\n", frame_delta - 1);

        /* Invoke the recognition network to process the image data */
        if (net_process_image(net_data, thresh, objects) != 0) {
            fprintf(stderr, "Error from recognition network when processing image\n");
            done = 1;
        }

        /* Free the image data */
        net_free_image_data(net_data);

        /* Provide notification about objects */
        notify_objects(objects, last_frame);
    }

    return NULL;
}

/* Fetch all frames from OpenCV */
void fetch_frames(CvCapture *cap1, CvCapture *cap2) {
    int frame = 0;
    int rv;
    IplImage *image;
    void *net_data;
    void *old_data;
    CvCapture *cap;

    while (!done) {
        printf("Acquiring frame #%d\n", ++frame);

        /* Select the active capture device */
        cap = active_cam == 0 ? cap1 : cap2;

        /* This blocks until a frame is available, or EOS */
        image = cvQueryFrame(cap);
        if (!image) {
            done = 1;
            rv = pthread_cond_signal(&image_cv);
            assert(rv == 0);
            return;
        }
        assert(image->width == cap_width);
        assert(image->height == cap_height);

        /*
         * Get the data from the frame that the network needs
         * This serves two purposes:
         *   1. Ensure the data is saved in the format that the network uses
         *   2. Allow us to get another image, as we cannot reference the image
         *      after another call to cvQueryFrame()
         */
        net_data = net_get_image_data(image);
        if (net_data == NULL) {
            fprintf(stderr, "WARNING: Network unable to process data in frame #%d\n", frame);
            continue;
        }

        printf("Saving frame #%d for processing\n", frame);

        /* Ensure exclusive access */
        rv = pthread_mutex_lock(&image_lock);
        assert(rv == 0);

        /* Set the image data as the next to be used */
        old_data = next_image_data;
        next_image_data = net_data;
        next_frame = frame;

        /* Release exclusivity ASAP */
        rv = pthread_mutex_unlock(&image_lock);
        assert(rv == 0);

        /*
         * Allow network to free data that never got processed (because we
         * grabbed another frame first)
         */
        if (old_data != NULL)
            net_free_image_data(old_data);

        /* Notify detector that there might be a new frame */
        rv = pthread_cond_signal(&image_cv);
        assert(rv == 0);
    }
}

/* Main program */
int main(int argc, char **argv) {
    /* Process command-line options */
    parse_options(argc, argv);

    /* Pass along remaining arguments to recognition network */
    if (net_parse_arguments(argc - optind, &argv[optind]) != 0) {
        fprintf(stderr, "Object recognition network failed to process arguments\n");
        exit(1);
    }

    /* Open data log, if applicable */
    if (data_log_fn != NULL) {
        data_log_fp = fopen(data_log_fn, "w");
        if (data_log_fp == NULL) {
            fprintf(stderr, "Unable to open data log (%s) for writing\n", data_log_fn);
            exit(1);
        }
    }

    /* Default to the primary camera -- *before* we start streaming */
    if (camera2_dev >= 0)
        change_camera(active_cam);

    /* Build up GStreamer pipepline */
    char *gstreamer_fmt = "uvch264src device=/dev/video%d entropy=cabac post-previews=false rate-control=vbr initial-bitrate=%d peak-bitrate=%d average-bitrate=%d iframe-period=%d auto-start=true name=src src.vfsrc ! queue ! tee name=t ! queue ! image/jpeg, width=%d, height=%d, framerate=%s ! avimux ! filesink location=%s t. ! jpegdec ! videoconvert ! appsink src.vidsrc ! queue ! video/x-h264, width=%d, height=%d, framerate=%s, profile=high, stream-format=byte-stream ! h264parse ! video/x-h264, stream-format=avc ! rtph264pay ! udpsink host=%s port=%d bind-port=%d";
    char gstreamer_cmd[1024] = { '\0' };
    if (opt_replay) {
        char *ext = strrchr(video_filename, '.');

        if (ext != NULL && strcmp(ext, ".jpg") == 0)
            snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "multifilesrc location=%s caps=\"image/jpeg, framerate=%s\" ! tee name=t ! queue ! rtpjpegpay ! udpsink host=%s port=%d t. ! jpegdec ! videoconvert ! appsink", video_filename, cap_fps, oper_host, video_port);
        else
            snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), "filesrc location=%s ! avidemux ! tee name=t ! queue ! rtpjpegpay ! udpsink host=%s port=%d t. ! jpegdec ! videoconvert ! appsink", video_filename, oper_host, video_port);
    } else {
        char buf[512];
        if (strchr(video_filename, '%') != NULL) {
            snprintf(buf, sizeof(buf), video_filename, 0);
        } else {
            strncpy(buf, video_filename, sizeof(buf));
        }
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), gstreamer_fmt, camera_dev, bitrate, bitrate, bitrate, iframe_ms, cap_width, cap_height, cap_fps, buf, stream_width, stream_height, stream_fps, oper_host, video_port, cam_port);
    }

    /* Use GStreamer to acquire video feeds */
    printf("Connecting to primary camera with GStreamer (%s)...\n", gstreamer_cmd);
    CvCapture *cap = cvCreateFileCaptureWithPreference(gstreamer_cmd, CV_CAP_GSTREAMER);
    if (cap == NULL) {
        fprintf(stderr, "Couldn't connect to primary camera with GStreamer\n");
        exit(1);
    }

    /* If using a second camera, also acquire that feed */
    CvCapture *cap2 = NULL;
    if (camera2_dev >= 0) {
        char buf[512];
        if (strchr(video_filename, '%') != NULL) {
            snprintf(buf, sizeof(buf), video_filename, 1);
        } else {
            strncpy(buf, video_filename, sizeof(buf));
        }
        snprintf(gstreamer_cmd, sizeof(gstreamer_cmd), gstreamer_fmt, camera2_dev, bitrate, bitrate, bitrate, iframe_ms, cap_width, cap_height, cap_fps, buf, stream_width, stream_height, stream_fps, oper_host, video_port, cam2_port);
        printf("Connecting to secondary camera with GStreamer (%s)...\n", gstreamer_cmd);
        cap2 = cvCreateFileCaptureWithPreference(gstreamer_cmd, CV_CAP_GSTREAMER);
        if (cap2 == NULL) {
            fprintf(stderr, "Couldn't connect to secondary camera with GStreamer\n");
            exit(1);
        }
    }
    printf("Connected to GStreamer\n");

    /* Kick off the AI network magic */
    printf("Preparing object detection network...\n");
    net_prepare(opt_gpu);

    /* Prepare the IP network */
    ip_network_init();

    /* Start camera monitor asynchronously */
    pthread_t camera_thread;
    if (camera2_dev >= 0) {
        printf("Starting camera monitor thread...\n");
        if (pthread_create(&camera_thread, NULL, camera_monitor, NULL)) {
            fprintf(stderr, "Thread creation failed\n");
            exit(1);
        }
    }

    /* Start detector asynchronously */
    pthread_t detect_thread;
    printf("Starting detector thread...\n");
    if (pthread_create(&detect_thread, NULL, detect_thread_impl, NULL)) {
        fprintf(stderr, "Thread creation failed\n");
        exit(1);
    }

    /* Process all of the frames */
    printf("Fetching frames from source...\n");
    fetch_frames(cap, cap2);

    /* Clean up */
    printf("Waiting for termination...\n");
    pthread_join(detect_thread, 0);
    if (camera2_dev >= 0)
        pthread_join(camera_thread, 0);
    cvReleaseCapture(&cap);
    if (cap2 != NULL)
        cvReleaseCapture(&cap2);
    if (data_log_fp != NULL)
        fclose(data_log_fp);

    return 0;
}
