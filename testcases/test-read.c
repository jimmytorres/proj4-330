#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdbool.h>
#include "common.h"

/* 
 * Description: This test writes a short message to the USB
 * device using bwrite and reads the message back using 'dd'.
 */
#define BLOCKSIZE   512

int main (int argc, char* argv[]) {
    printf("Testcase: read\n");

    if (!open_kmod()) {
        printf("error: could not open kmod chardev.\n"); 
        return 0;
    }
    
    char* device = argv[1];
    char* msg_send = malloc(BLOCKSIZE);
    snprintf(msg_send, BLOCKSIZE, "Goodbye from CSE330!\n");
    bwrite_using_dd(device, msg_send, BLOCKSIZE);

    char* msg_recv = malloc(BLOCKSIZE);
    bread(msg_recv, BLOCKSIZE);

    if (!verify_data(msg_send, msg_recv, BLOCKSIZE)) {
        printf("[X] FAILED \n");
        printf("[.]     Expected: %s\n", msg_send);
        printf("[.]     Received: %s\n", msg_recv);
        return -1;
    }

    close_kmod();
    printf("[*] PASSED \n");
    printf("[.]     Message: %s\n", msg_recv);
    return 0;
}