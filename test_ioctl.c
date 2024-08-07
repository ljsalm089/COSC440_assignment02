#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "user_common.h"

#define TAG "test_ioctl"
#define FILE_PATH "/dev/asgn1"

#define DEMO_COMMAND _IO('D', 1)
#define MAX_COMMAND _IOR('M', 2, int)

int max_process = 10;

int read_from_file(char *file_path) {
    char buff[1024];
    int fd = open(file_path, O_RDONLY);
    pid_t pid = getpid();
    extern int errno;

    if (fd == -1) {
        DEBUG(TAG, "Unable to open file in process (%d): %s", pid, strerror(errno));
        return 1;
    }

    DEBUG(TAG, "Open file success in process: %d", pid);

    int read_count = 0;
    while (read(fd, buff, 1024) > 0) {
        DEBUG(TAG, "Read from file (%d) in process: %d", read_count, pid);
        read_count ++;
        sleep(1);
    }

    close(fd);

    return 0;
}

int main(int argc, char **argv) {
    INFO(TAG, "hello, world!");

    pid_t pid;
    int status;
    extern int errno;

    char * file_path = FILE_PATH;
    if (argc > 1)
        file_path = argv[1];
    DEBUG(TAG, "file path: %s", file_path);

    for(int index = 0; index < max_process; index ++) {
        pid = fork();        
        if (pid < 0) {
            ERROR(TAG, "Unable to fork: %s", strerror(errno));
            return 1;
        }
        if (0 == pid) {
            // child
            int ret = read_from_file(file_path);
            if (2 == index || 6 == index) {
                int fd = open(file_path, O_RDWR);
                if (fd < 0)
                    return ret;

                int process_count = index == 6 ? 2 : 4;
                ret = ioctl(fd, MAX_COMMAND, &process_count);
                if (0 != ret) {
                    close(fd);
                    err_sys(TAG, "Fail (%d) to control the file: %s", ret, file_path);
                } else {
                    INFO(TAG, "update maximum process successfully.");
                }
                close(fd);
            }
            return 0;
        } else {
            // parent
        }
    }

    
    while ((pid = wait(&status)) > 0) {
        if (WIFEXITED(status)) {
            INFO(TAG, "Child with PID %d terminated with exit status %d", pid, status);
        } else if (WIFSIGNALED(status)) {
            INFO(TAG, "Child with PID %d terminated due to singal %d", pid, WTERMSIG(status));
        }
    }

    if (-1 == pid) {
        INFO(TAG, "Parent: No more children to wait for. All child processes have terminated.");
    }

    return 0;
}
