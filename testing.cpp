// g++ -Og -o testing `pkg-config --cflags opencv` testing.cpp `pkg-config --libs opencv` && ./testing
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;

#define CAM_CAP "video/x-raw"
#define STREAM_DEST_HOST "192.168.0.252"
#define STREAM_DEST_PORT "9999"
//#define GSTREAMER_COMMAND "videotestsrc ! progressreport update-freq=1 ! " CAM_CAP " ! tee name=t ! queue ! videoconvert ! queue ! jpegenc ! rtpjpegpay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! videoconvert ! appsink name=opencvsink"
#define GSTREAMER_COMMAND "multifilesrc location=/home/bbell/capimg/frame_\%04d.jpg caps=\"image/jpeg, framerate=30/1\" ! jpegdec ! videoconvert ! appsink"
//#define GSTREAMER_COMMAND "videotestsrc ! video/x-raw, width=640, height=480, framerate=30/1 ! videoconvert ! appsink"

const char *windowName;
const char *windowTitle = "My Testing";
int wait_ms = 10;
int thickness = 2;
int lineType = LINE_AA;
Scalar colors[] = {
    Scalar(0, 255, 255), // Cube
    Scalar(255, 0, 0), // Red bumper
    Scalar(0, 0, 255), // Blue bumper
};

Rect ourRect(float x, float y, float w, float h, Size size) {
    float left  = (x - w/2.) * size.width;
    float right = (x + w/2.) * size.height;
    float width = w * size.width;
    float height = h * size.height;

    return Rect(left, right, width, height);
}


int main(int argc, char **argv)
{
    Size size = Size(0, 0);

    stringstream ss;
    ss << argv[0] << "[" << getpid() << "]";
    windowName = ss.str().c_str();

    cout << "Connecting to GStreamer (" GSTREAMER_COMMAND ")..." << endl;
    VideoCapture cap(GSTREAMER_COMMAND, CV_CAP_GSTREAMER);
    if (!cap.isOpened()) {
        cout << "Couldn't connect to GStreamer" << endl;
        exit(1);
    }
    cout << "Connected to GStreamer" << endl;

    namedWindow(windowName, WINDOW_FREERATIO);
    setWindowTitle(windowName, windowTitle);

    Mat frame;
    while (true) {
        cap >> frame;
        if (frame.empty()) {
            break;
        }

        if (size.width == 0)
            size = frame.size();

        Rect myRect = ourRect(0.5, 0.75, 0.25, 0.35, size);

        rectangle(frame, myRect, colors[0], thickness, LINE_AA);

        imshow(windowName, frame);

        int key = waitKey(wait_ms);
        if ((key & 0xFF) == 27)
            break;
        if (getWindowProperty(windowName, WND_PROP_ASPECT_RATIO) < 0)
            break;
    }

    cout << "Exiting" << endl;
    exit(0);
}
