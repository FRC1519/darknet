/*
 * This code is based on the code that runs "darknet detector demo".
 * It is a trimmed down and edited version of darknet.c, detector.c, and demo.c
 */

#include "robot.h"
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

char *cfgfile;
char *weightfile;

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
void net_prepare(void) {
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

            /* TODO Implement real notification method via callback or data structure*/
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

int process_image(image img, float thresh) {
    /* Letterbox image to match the size of the network */
    static image boxed_image = { 0 };
    static int index = 0;

    if (img.w == net->w && img.h == net->h)
        boxed_image = img;
    else if (boxed_image.data == NULL)
        boxed_image = letterbox_image(img, net->w, net->h);
    else
        letterbox_image_into(img, net->w, net->h, boxed_image);

    float nms = .4;
    layer l = net->layers[net->n-1];
    float *prediction = network_predict(net, img.data);

    memcpy(predictions[index], prediction, l.outputs*sizeof(float));
    mean_arrays(predictions, avg_frames, l.outputs, avg);
    l.output = avg;
    if(l.type == DETECTION){
        get_detection_boxes(l, 1, 1, thresh, probs, boxes, 0);
    } else if (l.type == REGION){
        get_region_boxes(l, img.w, img.h, net->w, net->h, thresh, probs, boxes, 0, 0, 0, hier, 1);
    } else {
        error("Last layer must produce detections\n");
        return -1;
    }
    if (nms > 0) do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);
    index = (index + 1) % avg_frames;

    log_detections(img.w, img.h, detections, thresh, boxes, probs, names, classes);

    return 0;
}
