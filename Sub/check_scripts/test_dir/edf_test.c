#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{   
    // int num_procs = 3;
    // int dead_line[3] = {7, 24, 15};
    // int exectime[3] = {5, 6, 4};

    int parent_pid = getpid();

    // Set the scheduling policy to EDF
    deadline(parent_pid, 11);
    exec_time(parent_pid, 4);
    sched_policy(parent_pid, 0);
    while(1) {

    }
}
