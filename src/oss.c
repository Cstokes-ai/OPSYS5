#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

// Define constants
#define MAX_RESOURCES 5
#define MAX_INSTANCES 10
#define MAX_PROCESSES 18
typedef struct {
    int allocated[MAX_RESOURCES];
    int requested[MAX_RESOURCES];
    int pid;
} ProcessControlBlock;

typedef struct {
    int available[MAX_RESOURCES];
    int allocation[MAX_PROCESSES][MAX_RESOURCES];
    int request[MAX_PROCESSES][MAX_RESOURCES];
} ResourceDescriptor;

typedef struct {
    long mtype;
    int pid;
    int resource;
    int quantity;
    int request; // 1 for request, 0 for release
} Message;

int shmid_clock, shmid_resources, msqid;
int *shared_clock;
ResourceDescriptor *resources;
FILE *log_file; // Log file pointer

void cleanup() {
    if (log_file) fclose(log_file); // Close log file
    shmctl(shmid_clock, IPC_RMID, NULL);
    shmctl(shmid_resources, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
}

void signal_handler(int sig) {
    cleanup();
    exit(0);
}

void increment_clock(int increment) {
    shared_clock[1] += increment; // Increment nanoseconds
    if (shared_clock[1] >= 1000000000) {
        shared_clock[0] += 1; // Increment seconds
        shared_clock[1] -= 1000000000;
    }
}

void log_resource_table() {
    fprintf(log_file, "Current Resource Table at time %d:%d\n", shared_clock[0], shared_clock[1]);
    fprintf(log_file, "Available Resources:\n");
    for (int i = 0; i < MAX_RESOURCES; i++) {
        fprintf(log_file, "R%d: %d ", i, resources->available[i]);
    }
    fprintf(log_file, "\n");

    fprintf(log_file, "Allocation Table:\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (resources->allocation[i][0] != -1) { // Process is active
            fprintf(log_file, "P%d: ", i);
            for (int j = 0; j < MAX_RESOURCES; j++) {
                fprintf(log_file, "%d ", resources->allocation[i][j]);
            }
            fprintf(log_file, "\n");
        }
    }
    fprintf(log_file, "\n");
}

void deadlock_detection() {
    int work[MAX_RESOURCES];
    int finish[MAX_PROCESSES] = {0};
    int deadlocked[MAX_PROCESSES] = {0};
    int i, j;

    // Initialize work array
    for (i = 0; i < MAX_RESOURCES; i++) {
        work[i] = resources->available[i];
    }

    // Check for deadlock
    int progress = 1;
    while (progress) {
        progress = 0;
        for (i = 0; i < MAX_PROCESSES; i++) {
            if (!finish[i]) {
                int can_finish = 1;
                for (j = 0; j < MAX_RESOURCES; j++) {
                    if (resources->request[i][j] > work[j]) {
                        can_finish = 0;
                        break;
                    }
                }
                if (can_finish) {
                    for (j = 0; j < MAX_RESOURCES; j++) {
                        work[j] += resources->allocation[i][j];
                    }
                    finish[i] = 1;
                    progress = 1;
                }
            }
        }
    }

    // Identify deadlocked processes
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (!finish[i] && resources->allocation[i][0] != -1) {
            deadlocked[i] = 1;
            fprintf(log_file, "Process %d is deadlocked\n", i);
        }
    }

    // Resolve deadlock by terminating processes
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (deadlocked[i]) {
            fprintf(log_file, "Terminating process %d to resolve deadlock\n", i);
            for (j = 0; j < MAX_RESOURCES; j++) {
                resources->available[j] += resources->allocation[i][j];
                resources->allocation[i][j] = 0;
                resources->request[i][j] = 0;
            }
            resources->allocation[i][0] = -1; // Mark process as terminated
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);

    // Default values for parameters
    int n = 1; // Number of processes
    int s = 1; // Simulation time in seconds

    // Parse command-line arguments
    int opt;
    while ((opt = getopt(argc, argv, "n:s:")) != -1) {
        switch (opt) {
            case 'n':
                n = atoi(optarg);
                if (n < 1 || n > MAX_PROCESSES) {
                    fprintf(stderr, "Error: -n must be between 1 and %d\n", MAX_PROCESSES);
                    exit(1);
                }
                break;
            case 's':
                s = atoi(optarg);
                if (s < 1) {
                    fprintf(stderr, "Error: -s must be at least 1 second\n");
                    exit(1);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s [-n num_processes] [-s simulation_time]\n", argv[0]);
                exit(1);
        }
    }

    // Print parsed parameters for debugging
    printf("Number of processes: %d\n", n);
    printf("Simulation time: %d seconds\n", s);

    // Open log file
    log_file = fopen("oss.log", "w");
    if (!log_file) {
        perror("Failed to open log file");
        exit(1);
    }

    // Initialize shared memory and message queue
    shmid_clock = shmget(IPC_PRIVATE, sizeof(int) * 2, IPC_CREAT | 0666);
    shmid_resources = shmget(IPC_PRIVATE, sizeof(ResourceDescriptor), IPC_CREAT | 0666);
    msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);

    shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    resources = (ResourceDescriptor *)shmat(shmid_resources, NULL, 0);

    shared_clock[0] = 0; // Seconds
    shared_clock[1] = 0; // Nanoseconds

    // Initialize resources
    for (int i = 0; i < MAX_RESOURCES; i++) {
        resources->available[i] = MAX_INSTANCES;
        for (int j = 0; j < MAX_PROCESSES; j++) {
            resources->allocation[j][i] = 0;
            resources->request[j][i] = 0;
        }
    }

    // Main loop
    while (shared_clock[0] < s) { // Run for the specified simulation time
        increment_clock(10000); // Increment clock by 10ms

        // Check for messages from user processes
        Message msg;
        while (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, IPC_NOWAIT) != -1) {
            if (msg.request == 1) { // Resource request
                if (resources->available[msg.resource] >= msg.quantity) {
                    resources->available[msg.resource] -= msg.quantity;
                    resources->allocation[msg.pid][msg.resource] += msg.quantity;
                    fprintf(log_file, "Granted resource %d to process %d\n", msg.resource, msg.pid);
                } else {
                    resources->request[msg.pid][msg.resource] += msg.quantity;
                    fprintf(log_file, "Process %d is waiting for resource %d\n", msg.pid, msg.resource);
                }
            } else { // Resource release
                resources->available[msg.resource] += msg.quantity;
                resources->allocation[msg.pid][msg.resource] -= msg.quantity;
                fprintf(log_file, "Process %d released resource %d\n", msg.pid, msg.resource);
            }
        }

        // Periodically check for deadlock
        if (shared_clock[0] % 1 == 0 && shared_clock[1] == 0) {
            deadlock_detection();
        }

        // Log resource table every 500ms
        if (shared_clock[1] % 500000000 == 0) {
            log_resource_table();
        }

        usleep(1000); // Sleep for 1ms
    }

    cleanup();
    return 0;
}