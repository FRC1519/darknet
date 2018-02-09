// g++ -Og -o simpletestcpp `pkg-config --cflags opencv` simpletest.cpp `pkg-config --libs opencv` && GST_DEBUG=3 ./simpletestcpp
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;

#define CAM_CAP "video/x-raw"
#define STREAM_DEST_HOST "192.168.0.252"
#define STREAM_DEST_PORT "9999"
#define GSTREAMER_COMMAND "videotestsrc ! progressreport update-freq=1 ! " CAM_CAP " ! tee name=t ! queue ! videoconvert ! queue ! jpegenc ! rtpjpegpay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! videoconvert ! appsink name=opencvsink"
//#define GSTREAMER_COMMAND "videotestsrc ! progressreport update-freq=1 ! " CAM_CAP " ! tee name=t ! queue ! videoconvert ! queue ! x264enc tune=zerolatency bitrate=256 speed-preset=superfast ! rtph264pay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! videoconvert ! appsink name=opencvsink"


int main(int argc, char **argv)
{
    cout << "Connecting to GStreamer (" GSTREAMER_COMMAND ")..." << endl;
    VideoCapture cap(GSTREAMER_COMMAND);
    if (!cap.isOpened()) {
        cout << "Couldn't connect to GStreamer" << endl;
        exit(1);
    }
    cout << "Connected to GStreamer" << endl;

    Mat frame;
    while (true) {
        cap >> frame;
        if (frame.empty()) {
            break;
        }

        imshow(argv[0], frame);
        int key = waitKey(10);
        if (key == 27) {
            break;
        }
    }

    cout << "Exiting" << endl;
    exit(0);
}
