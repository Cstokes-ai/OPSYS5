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
#include <stdbool.h>

#define MAX_RESOURCES 5
#define MAX_INSTANCES 10
#define MAX_PROCESSES 18

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
    int request;
} Message;

int shmid_clock, shmid_resources, msqid;
int *shared_clock;
ResourceDescriptor *resources;
FILE *log_file;

void cleanup() {
    if (log_file) fclose(log_file);
    shmctl(shmid_clock, IPC_RMID, NULL);
    shmctl(shmid_resources, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
}

void signal_handler(int sig) {
    cleanup();
    exit(0);
}

void increment_clock(int increment) {
    shared_clock[1] += increment;
    if (shared_clock[1] >= 1000000000) {
        shared_clock[0] += 1;
        shared_clock[1] -= 1000000000;
    }
}

void log_resource_table() {
    fprintf(log_file, "Current system resources at time %d:%d\n", shared_clock[0], shared_clock[1]);
    fprintf(log_file, "R0 R1 R2 R3 R4\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        bool active = false;
        for (int j = 0; j < MAX_RESOURCES; j++) {
            if (resources->allocation[i][j] > 0) {
                active = true;
                break;
            }
        }
        if (active) {
            fprintf(log_file, "P%d ", i);
            for (int j = 0; j < MAX_RESOURCES; j++) {
                fprintf(log_file, "%d ", resources->allocation[i][j]);
            }
            fprintf(log_file, "\n");
        }
    }
    fprintf(log_file, "\n");
    fflush(log_file);
}

bool req_lt_avail(const int *req, const int *avail, const int pnum, const int num_res) {
    for (int i = 0; i < num_res; i++) {
        if (req[pnum * num_res + i] > avail[i]) return false;
    }
    return true;
}

bool deadlock(const int *available, const int m, const int n, const int *request, const int *allocated) {
    int work[m];
    bool finish[n];

    for (int i = 0; i < m; i++) work[i] = available[i];
    for (int i = 0; i < n; i++) finish[i] = false;

    int p;
    for (p = 0; p < n; p++) {
        if (!finish[p] && req_lt_avail(request, work, p, m)) {
            finish[p] = true;
            for (int i = 0; i < m; i++) {
                work[i] += allocated[p * m + i];
            }
            p = -1;
        }
    }

    for (p = 0; p < n; p++) if (!finish[p]) return true;
    return false;
}

void deadlock_detection() {
    int m = MAX_RESOURCES, n = MAX_PROCESSES;
    int allocated_flat[MAX_PROCESSES * MAX_RESOURCES];
    int request_flat[MAX_PROCESSES * MAX_RESOURCES];

    for (int i = 0; i < MAX_PROCESSES; i++) {
        for (int j = 0; j < MAX_RESOURCES; j++) {
            allocated_flat[i * MAX_RESOURCES + j] = resources->allocation[i][j];
            request_flat[i * MAX_RESOURCES + j] = resources->request[i][j];
        }
    }

    fprintf(log_file, "Master running deadlock detection at time %d:%d\n", shared_clock[0], shared_clock[1]);

    if (deadlock(resources->available, m, n, request_flat, allocated_flat)) {
        fprintf(log_file, "Master running deadlock detection at time %d:%d: Deadlocks detected\n", shared_clock[0], shared_clock[1]);
        for (int i = 0; i < MAX_PROCESSES; i++) {
            bool is_deadlocked = true;
            for (int j = 0; j < MAX_RESOURCES; j++) {
                if (resources->request[i][j] > resources->available[j]) {
                    is_deadlocked = false;
                    break;
                }
            }
            if (is_deadlocked) {
                fprintf(log_file, "Master terminating P%d to remove deadlock\n", i);
                for (int j = 0; j < MAX_RESOURCES; j++) {
                    if (resources->allocation[i][j] > 0) {
                        fprintf(log_file, "Resources released: R%d:%d\n", j, resources->allocation[i][j]);
                        resources->available[j] += resources->allocation[i][j];
                        resources->allocation[i][j] = 0;
                    }
                }
                fprintf(log_file, "Process P%d terminated\n", i);
            }
        }
    } else {
        fprintf(log_file, "Master running deadlock detection at time %d:%d: No deadlocks detected\n", shared_clock[0], shared_clock[1]);
    }
    fflush(log_file);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);

    int n = 1, s = 1;
    int opt;
    while ((opt = getopt(argc, argv, "n:s:")) != -1) {
        switch (opt) {
            case 'n': n = atoi(optarg); break;
            case 's': s = atoi(optarg); break;
            default:
                fprintf(stderr, "Usage: %s [-n num_processes] [-s simulation_time]\n", argv[0]);
                exit(1);
        }
    }

    log_file = fopen("oss.log", "w");
    if (!log_file) { perror("Failed to open log file"); exit(1); }

    shmid_clock = shmget(IPC_PRIVATE, sizeof(int) * 2, IPC_CREAT | 0666);
    shmid_resources = shmget(IPC_PRIVATE, sizeof(ResourceDescriptor), IPC_CREAT | 0666);
    msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0666);

    shared_clock = (int *)shmat(shmid_clock, NULL, 0);
    resources = (ResourceDescriptor *)shmat(shmid_resources, NULL, 0);

    shared_clock[0] = 0; shared_clock[1] = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        resources->available[i] = MAX_INSTANCES;
        for (int j = 0; j < MAX_PROCESSES; j++) {
            resources->allocation[j][i] = 0;
            resources->request[j][i] = 0;
        }
    }

    while (shared_clock[0] < s) {
        increment_clock(10000);
        Message msg;
        while (msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 0, IPC_NOWAIT) != -1) {
            if (msg.request == 1) {
                fprintf(log_file, "Master has detected Process P%d requesting R%d at time %d:%d\n",
                        msg.pid, msg.resource, shared_clock[0], shared_clock[1]);

                if (resources->available[msg.resource] >= msg.quantity) {
                    resources->available[msg.resource] -= msg.quantity;
                    resources->allocation[msg.pid][msg.resource] += msg.quantity;
                    fprintf(log_file, "Master granting P%d request R%d at time %d:%d\n",
                            msg.pid, msg.resource, shared_clock[0], shared_clock[1]);
                } else {
                    resources->request[msg.pid][msg.resource] += msg.quantity;
                    fprintf(log_file, "Master: no instances of R%d available, P%d added to wait queue at time %d:%d\n",
                            msg.resource, msg.pid, shared_clock[0], shared_clock[1]);
                }
            } else {
                resources->available[msg.resource] += msg.quantity;
                resources->allocation[msg.pid][msg.resource] -= msg.quantity;
                fprintf(log_file, "Master has acknowledged Process P%d releasing R%d at time %d:%d\n",
                        msg.pid, msg.resource, shared_clock[0], shared_clock[1]);
                fprintf(log_file, "Resources released: R%d:%d\n", msg.resource, msg.quantity);
            }
            fflush(log_file);
        }

        if (shared_clock[0] % 1 == 0 && shared_clock[1] == 0) {
            deadlock_detection();
        }

        if (shared_clock[1] % 500000000 == 0) {
            log_resource_table();
        }

        usleep(1000);
    }

    cleanup();
    return 0;
}
