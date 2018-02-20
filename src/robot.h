#include <stdint.h>
#include <opencv2/core/types_c.h>

/*
 * All possible object type
 * Ensure that this data matches available configuration files, etc.
 */
typedef enum object_types {
    OBJ_NONE,
    OBJ_CUBE,
    OBJ_SCALE_CENTER,
    OBJ_SCALE_BLUE,
    OBJ_SCALE_RED,
    OBJ_SWITCH_RED,
    OBJ_SWITCH_BLUE,
    OBJ_PORTAL_RED,
    OBJ_PORTAL_BLUE,
    OBJ_EXCHANGE_RED,
    OBJ_EXCHANGE_BLUE,
    OBJ_BUMPERS_RED,
    OBJ_BUMPERS_BLUE,
    OBJ_EOL,
} object_type;

typedef struct object_location {
    object_type type;
    float x;
    float y;
    float width;
    float height;
    float probability;
} object_location;

#define MAX_OBJECTS_PER_FRAME 20

#define MAYHEM_MAGIC 0x1519B0B4

typedef struct object_dg {
    uint32_t type;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t probability;
} __attribute__((packed)) object_dg;

typedef struct datagram {
    uint32_t magic;
    uint32_t frame_number;
    uint64_t timestamp;
    object_dg object_data[MAX_OBJECTS_PER_FRAME];
} __attribute__((packed)) datagram;

/* Object detection API */
int   net_parse_arguments(int argc, char **argv);
void  net_prepare(int gpu_idx);
void *net_get_image_data(IplImage *cv_image);
void  net_free_image_data(void *image_data);
int   net_process_image(void *image_data, float thresh, object_location *objects);
