/* This file is a simple test of receiving object location updates by the OI */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "robot.h"

int main(int argc, char **argv) {
    int port = 5810;
    int sock;
    int reuseaddr;
    struct sockaddr_in svr_addr;
    int rv;
    object_location obj_loc;
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

        printf("Received frame %" PRIu32 " sent at %" PRIu64 "\n", be32toh(data.frame_number), be32toh(data.timestamp));
        for (int i = 0; i < MAX_OBJECTS_PER_FRAME; i++) {
            obj_loc.type = be32toh(data.object_data[i].type);
            if (obj_loc.type == OBJ_NONE)
                break;
            obj_loc.x = (float)be32toh(data.object_data[i].x) / INT32_MAX;
            obj_loc.y = (float)be32toh(data.object_data[i].y) / INT32_MAX;
            obj_loc.width = (float)be32toh(data.object_data[i].width) / INT32_MAX;
            obj_loc.height = (float)be32toh(data.object_data[i].height) / INT32_MAX;
            obj_loc.probability = (float)be32toh(data.object_data[i].probability) / INT32_MAX;
            printf("OBJECT FOUND in frame #%" PRIu32 ": Type %d @ %.02f x %.02f [ %.02f x %.02f ], %.02f%%\n", be32toh(data.frame_number), obj_loc.type, obj_loc.x, obj_loc.y, obj_loc.width, obj_loc.height, obj_loc.probability);
        }
    }

    return 0;
}
