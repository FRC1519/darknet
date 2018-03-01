// g++ -Og -o testing `pkg-config --cflags opencv` testing.cpp `pkg-config --libs opencv` && ./testing
// g++ -Og -o testing `pkg-config --cflags opencv` testing.cpp -static `pkg-config --libs opencv libv4l1 libv4l2 libv4lconvert libjpeg libgphoto2 libgphoto2_port libtiff-4 liblzma zlib libpng libwebp libexif libdc1394-2 gstreamer-1.0` -pthread -ldl -ljbig -lltdl 

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <opencv2/opencv.hpp>

#include "robot.h"

using namespace std;
using namespace cv;

#define GSTREAMER_COMMAND "multifilesrc location=/home/bbell/capimg/frame_\%04d.jpg caps=\"image/jpeg, framerate=15/1\" ! jpegdec ! videoconvert ! appsink"

const char *windowTitle = "My Testing";
int wait_ms = 10;
int thickness = 2;
int lineType = LINE_AA;

pthread_mutex_t obj_lock  = PTHREAD_MUTEX_INITIALIZER;
int obj_frame = 0;
int obj_count = 0;
long obj_timestamp = 0;
object_location obj_loc[MAX_OBJECTS_PER_FRAME];

float minThreshold = 0.15; // Minimum probability to draw
float maxThreshold = 0.75; // Probability above which we consider object basically certain
float dimmingRange = 0.3; // Amount of color that is subject to dimming

Rect ourRect(float x, float y, float w, float h, Size size) {
    float left = (x - w / 2.) * size.width;
    float top = (y - h / 2.) * size.height;
    float width = w * size.width;
    float height = h * size.height;

    return Rect(left, top, width, height);
}

void *listener(void *arg) {
    int port = 5810;
    int sock;
    int reuseaddr;
    struct sockaddr_in svr_addr;
    int rv;
    ssize_t n;
    datagram data;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Failed to create datagram socket");
        exit(1);
    }

    rv = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
    if (rv < 0) {
        perror("Failed to configure socket to reuse address");
        exit(1);
    }

    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    svr_addr.sin_port = htons(port);

    rv = bind(sock, (struct sockaddr *)&svr_addr, sizeof(svr_addr));
    if (rv < 0) {
        perror("Failed to bind to server port");
        exit(1);
    }

    printf("Awaiting notification of objects...\n");
    while (1) {
        n = recv(sock, &data, sizeof(data), 0);
        if (n < 0) {
            perror("Failed receiving datagram");
            exit(1);
        }
        if (n != sizeof(data)) {
            fprintf(stderr, "WARNING: received %zd bytes instead of %zu bytes\n", n, sizeof(data));
            continue;
        }
        if (be32toh(data.magic) != MAYHEM_MAGIC) {
            fprintf(stderr, "WARNING: malformed datagram received\n");
            continue;
        }

        pthread_mutex_lock(&obj_lock);
        obj_frame = be32toh(data.frame_number);
        obj_timestamp = be64toh(data.timestamp);
        cout << "Received frame " << obj_frame << " sent at " << obj_timestamp << endl;

        for (obj_count = 0; obj_count < MAX_OBJECTS_PER_FRAME; obj_count++) {
            obj_loc[obj_count].type = (object_type)be32toh(data.object_data[obj_count].type);
            if (obj_loc[obj_count].type == OBJ_NONE)
                break;
            obj_loc[obj_count].x = (float)be32toh(data.object_data[obj_count].x) / INT32_MAX;
            obj_loc[obj_count].y = (float)be32toh(data.object_data[obj_count].y) / INT32_MAX;
            obj_loc[obj_count].width = (float)be32toh(data.object_data[obj_count].width) / INT32_MAX;
            obj_loc[obj_count].height = (float)be32toh(data.object_data[obj_count].height) / INT32_MAX;
            obj_loc[obj_count].probability = (float)be32toh(data.object_data[obj_count].probability) / INT32_MAX;
            printf("OBJECT FOUND in frame #%" PRIu32 ": Type %d @ %.02f x %.02f [ %.02f x %.02f ], %.02f%%\n", obj_frame, obj_loc[obj_count].type, obj_loc[obj_count].x, obj_loc[obj_count].y, obj_loc[obj_count].width, obj_loc[obj_count].height, obj_loc[obj_count].probability);
        }
        pthread_mutex_unlock(&obj_lock);
    }
}

// Pick a color based on the object type a probability
Scalar pickColor(object_type objType, float probability) {
    int type = (int)objType;
    Scalar color = Scalar(150, 150, 150); // Default to a medium gray

    // High certain object types with associated colors
    switch (objType) {
        case OBJ_CUBE: color = Scalar(0, 255, 255); break;
        case OBJ_BUMPERS_RED: color = Scalar(255, 0, 0); break;
        case OBJ_BUMPERS_BLUE: color = Scalar(0, 0, 255); break;
        case OBJ_BOX: color = Scalar(0, 255, 0); break;
        default: break;
    }

    // Adjust intensity of color based on probability
    if (probability < maxThreshold)
        color *= 1.0 - (1.0 - ((probability - minThreshold) / (maxThreshold - minThreshold))) * dimmingRange;

    return color;
}

void draw_objs(Mat &frame) {
    static Size size = Size(0, 0);

    if (size.width == 0)
        size = frame.size();

    pthread_mutex_lock(&obj_lock);
    for (int i = 0; i < obj_count; i++) {
        // Skip unlikely objects
        if (obj_loc[i].probability < minThreshold)
            continue;

        printf("DRAWING OBJECT in frame #%" PRIu32 ": Type %d @ %.02f x %.02f [ %.02f x %.02f ], %.02f%%\n", obj_frame, obj_loc[i].type, obj_loc[i].x, obj_loc[i].y, obj_loc[i].width, obj_loc[i].height, obj_loc[i].probability);

        Scalar color = pickColor(obj_loc[i].type, obj_loc[i].probability);

        rectangle(frame, ourRect(obj_loc[i].x, obj_loc[i].y, obj_loc[i].width, obj_loc[i].height, size), color, thickness, LINE_AA);
    }
    pthread_mutex_unlock(&obj_lock);
}

int main(int argc, char **argv) {
    // Listen for detected object information in a separate thread
    pthread_t listener_thread;
    cout << "Starting listener..." << endl;
    if (pthread_create(&listener_thread, NULL, listener, NULL)) {
        cerr << "Thread creation failed" << endl;
        exit(1);
    }

    // Open a connection to the video feed
    cout << "Connecting to GStreamer (" GSTREAMER_COMMAND ")..." << endl;
    VideoCapture cap(GSTREAMER_COMMAND, CV_CAP_GSTREAMER);
    if (!cap.isOpened()) {
        cout << "Couldn't connect to GStreamer" << endl;
        exit(1);
    }
    cout << "Connected to GStreamer" << endl;

    // Build a unique window name
    char windowName[30];
    snprintf(windowName, sizeof(windowName), "WINDOW[%d]", getpid());

    // Create window to display video feed
    namedWindow(windowName, WINDOW_KEEPRATIO);
    setWindowTitle(windowName, windowTitle);

    Mat frame;
    while (true) {
        // Obtain next frame (blocks waiting for frame)
        cap >> frame;

        // Abort if feed has terminated
        if (frame.empty())
            break;

        // Augment frame with most recent knowledge of images
        draw_objs(frame);

        // Show the augmented frame in the window
        imshow(windowName, frame);

        // Abort if escape key has been hit
        int key = waitKey(wait_ms);
        if ((key & 0xFF) == 27)
            break;

        // Abort if window has been closed
        if (getWindowProperty(windowName, WND_PROP_ASPECT_RATIO) < 0)
            break;
    }

    exit(0);
}
