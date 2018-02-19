// gcc -Og -o simpletest `pkg-config --cflags opencv` simpletest.c `pkg-config --libs opencv` && GST_DEBUG=3 ./simpletest
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "opencv2/videoio/videoio_c.h"

#define CAM_CAP "video/x-raw"
#define STREAM_DEST_HOST "192.168.0.252"
#define STREAM_DEST_PORT "9999"
//#define GSTREAMER_COMMAND "videotestsrc ! progressreport update-freq=1 ! " CAM_CAP " ! tee name=t ! queue ! videoconvert ! queue ! jpegenc ! rtpjpegpay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! appsink"
//#define GSTREAMER_COMMAND "videotestsrc ! " CAM_CAP " ! queue ! videoconvert ! queue ! jpegenc ! rtpjpegpay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT
//#define GSTREAMER_COMMAND "videotestsrc ! progressreport update-freq=1 ! " CAM_CAP " ! tee name=t ! queue ! videoconvert ! queue ! x264enc tune=zerolatency bitrate=256 speed-preset=superfast ! rtph264pay ! udpsink host=" STREAM_DEST_HOST " port=" STREAM_DEST_PORT " t. ! appsink"
//#define GSTREAMER_COMMAND "filesrc location=/home/bbell/Downloads/capsaved.avi ! avidemux ! tee name=t ! queue ! rtpjpegpay ! udpsink host=192.168.0.252 port=9999 t. ! jpegdec ! videoconvert ! appsink"
//#define GSTREAMER_COMMAND "filesrc location=/home/bbell/Downloads/capsaved.avi ! avidemux ! jpegdec ! videoconvert ! video/x-raw, format=(string)BGR ! appsink"
#define GSTREAMER_COMMAND "multifilesrc location=/home/bbell/capimg/frame_\%04d.jpg caps=\"image/jpeg, framerate=30/1\" ! jpegdec ! videoconvert ! appsink"
//#define GSTREAMER_COMMAND "videotestsrc ! videoconvert ! appsink"


int main(int argc, char **argv)
{
    CvCapture *cap;

    printf("Connecting to GStreamer (%s)... \n", GSTREAMER_COMMAND);
    cap = cvCreateFileCaptureWithPreference(GSTREAMER_COMMAND, CV_CAP_GSTREAMER);
    if (!cap) {
        printf("Couldn't connect to GStreamer.\n");
        exit(1);
    }
    printf("Connected to GStreamer.\n");

    IplImage *image;
    int frameCount = 0;
    while ((image = cvQueryFrame(cap)) != NULL) {
        printf("Captured frame #%d\n", ++frameCount);
    }

    printf("Exiting\n");
    exit(0);
}
