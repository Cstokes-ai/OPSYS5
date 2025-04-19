#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <time.h>

// Define constants
#define MAX_RESOURCES 5
#define MAX_INSTANCES 10

typedef struct {
    long mtype;
    int pid;
    int resource;
    int quantity;
    int request; // 1 for request, 0 for release
} Message;

int main(int argc, char *argv[]) {
    int msqid = atoi(argv[1]);
    int shmid_clock = atoi(argv[2]);
    int pid = getpid();

    int *shared_clock = (int *)shmat(shmid_clock, NULL, 0);

    srand(time(NULL) ^ pid);

    while (1) {
        int action = rand() % 2; // 0 for release, 1 for request
        int resource = rand() % MAX_RESOURCES;
        int quantity = (rand() % MAX_INSTANCES) + 1;

        Message msg;
        msg.mtype = 1;
        msg.pid = pid;
        msg.resource = resource;
        msg.quantity = quantity;
        msg.request = action;

        if (action == 1) {
            printf("Process %d requesting %d of resource %d\n", pid, quantity, resource);
        } else {
            printf("Process %d releasing %d of resource %d\n", pid, quantity, resource);
        }

        msgsnd(msqid, &msg, sizeof(Message) - sizeof(long), 0);

        sleep(rand() % 3 + 1); // Sleep for 1-3 seconds

        // Check if process should terminate
        if (shared_clock[0] >= 1) {
            printf("Process %d terminating\n", pid);
            break;
        }
    }

    return 0;
}