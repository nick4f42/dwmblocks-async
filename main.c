#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>

#define LEN(arr) (sizeof(arr) / sizeof(arr[0]))
#define MAX(a, b) (a > b ? a : b)

typedef struct {
    unsigned int interval;
    unsigned int signal;
    char **argv;
} Block;

static const char *argsep = "--";

#define CLICKABLE_ARG "--clickable"
#define LEADING_DELIM_ARG "--leading-delim"
#define DELIMITER_ARG "--delimiter"
#define CMDLENGTH_ARG "--cmdlength"
#define DEBUG_ARG "--debug"

#define CMDLENGTH_MAX 4096

static int blockCount;
static Block *blocks;
static const char *delimiter = "  ";
static int cmdlength = 64;
static int clickableBlocks;
static int leadingDelim;

static int outputSize;
static char *outputs;
static int blockBufferSize;
static char *blockBuffer;
static char *statusBar[2];
static int (*pipes)[2];
static struct epoll_event *events;

static Display *dpy;
static Window root;
static unsigned short statusContinue = 1;
static struct epoll_event event;
static int timer = 0, timerTick = 0, maxInterval = 1;
static int signalFD;
static int epollFD;
static int execLock = 0;

void usage() {
    fputs("usage: dwmblocks [options] (-- interval signal prog arg...)...\n", stderr);
}

void (*writeStatus)();

int gcd(int a, int b) {
    int temp;
    while (b > 0) {
        temp = a % b;
        a = b;
        b = temp;
    }
    return a;
}

void closePipe(int *pipe) {
    close(pipe[0]);
    close(pipe[1]);
}

void execBlock(int i, const char *button) {
    // Ensure only one child process exists per block at an instance
    if (execLock & 1 << i) return;
    // Lock execution of block until current instance finishes execution
    execLock |= 1 << i;

    if (fork() == 0) {
        close(pipes[i][0]);
        dup2(pipes[i][1], STDOUT_FILENO);
        close(pipes[i][1]);

        if (button) setenv("BLOCK_BUTTON", button, 1);
        char **argv = blocks[i].argv;
        execvp(argv[0], argv);
        exit(EXIT_FAILURE);
    }
}

void execBlocks(unsigned int time) {
    for (int i = 0; i < blockCount; i++)
        if (time == 0 ||
            (blocks[i].interval != 0 && time % blocks[i].interval == 0))
            execBlock(i, NULL);
}

int getStatus(char *new, char *old) {
    strcpy(old, new);
    new[0] = '\0';

    for (int i = 0; i < blockCount; i++) {
        char *output = &outputs[i * outputSize];
        if (leadingDelim && *output ||
            !leadingDelim && *new && *output) {
            strcat(new, delimiter);
        }
        strcat(new, output);
    }
    return strcmp(new, old);
}

void updateBlock(int i) {
    char *output = &outputs[i * outputSize];
    int bytesRead = read(pipes[i][0], blockBuffer, blockBufferSize);

    // Trim UTF-8 string to desired length
    int count = 0, j = 0;
    while (blockBuffer[j] != '\n' && count < cmdlength) {
        count++;

        // Skip continuation bytes, if any
        char ch = blockBuffer[j];
        int skip = 1;
        while ((ch & 0xc0) > 0x80) ch <<= 1, skip++;
        j += skip;
    }

    // Cache last character and replace it with a trailing space
    char ch = blockBuffer[j];
    blockBuffer[j] = ' ';

    // Trim trailing spaces
    while (j >= 0 && blockBuffer[j] == ' ') j--;
    blockBuffer[j + 1] = 0;

    // Clear the pipe
    if (bytesRead == blockBufferSize) {
        while (ch != '\n' && read(pipes[i][0], &ch, 1) == 1)
            ;
    }

    if (clickableBlocks && bytesRead > 1 && blocks[i].signal > 0) {
        output[0] = blocks[i].signal;
        output++;
    }

    strcpy(output, blockBuffer);

    // Remove execution lock for the current block
    execLock &= ~(1 << i);
}

void debug() {
    // Only write out if text has changed
    if (!getStatus(statusBar[0], statusBar[1])) return;

    write(STDOUT_FILENO, statusBar[0], strlen(statusBar[0]));
    write(STDOUT_FILENO, "\n", 1);
}

int setupX() {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

    root = DefaultRootWindow(dpy);
    return 0;
}

void setRoot() {
    // Only set root if text has changed
    if (!getStatus(statusBar[0], statusBar[1])) return;

    XStoreName(dpy, root, statusBar[0]);
    XFlush(dpy);
}

void signalHandler() {
    struct signalfd_siginfo info;
    read(signalFD, &info, sizeof(info));
    unsigned int signal = info.ssi_signo;

    switch (signal) {
        case SIGALRM:
            // Schedule the next timer event and execute blocks
            alarm(timerTick);
            execBlocks(timer);

            // Wrap `timer` to the interval [1, `maxInterval`]
            timer = (timer + timerTick - 1) % maxInterval + 1;
            return;
        case SIGUSR1:
            // Update all blocks on receiving SIGUSR1
            execBlocks(0);
            return;
    }

    for (int j = 0; j < blockCount; j++) {
        if (blocks[j].signal == signal - SIGRTMIN) {
            char button[4];  // value can't be more than 255;
            sprintf(button, "%d", info.ssi_int & 0xff);
            execBlock(j, button);
            break;
        }
    }
}

void termHandler() { statusContinue = 0; }

void setupSignals() {
    sigset_t handledSignals;
    sigemptyset(&handledSignals);
    sigaddset(&handledSignals, SIGUSR1);
    sigaddset(&handledSignals, SIGALRM);

    // Append all block signals to `handledSignals`
    for (int i = 0; i < blockCount; i++)
        if (blocks[i].signal > 0)
            sigaddset(&handledSignals, SIGRTMIN + blocks[i].signal);

    // Create a signal file descriptor for epoll to watch
    signalFD = signalfd(-1, &handledSignals, 0);
    event.data.u32 = blockCount;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, signalFD, &event);

    // Block all realtime and handled signals
    for (int i = SIGRTMIN; i <= SIGRTMAX; i++) sigaddset(&handledSignals, i);
    sigprocmask(SIG_BLOCK, &handledSignals, NULL);

    // Handle termination signals
    signal(SIGINT, termHandler);
    signal(SIGTERM, termHandler);

    // Avoid zombie subprocesses
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, 0);
}

void statusLoop() {
    // Update all blocks initially
    raise(SIGALRM);

    while (statusContinue) {
        int eventCount = epoll_wait(epollFD, events, blockCount + 1, -1);
        for (int i = 0; i < eventCount; i++) {
            unsigned short id = events[i].data.u32;
            if (id < blockCount)
                updateBlock(id);
            else
                signalHandler();
        }

        if (eventCount != -1) writeStatus();
    }
}

void init() {
    epollFD = epoll_create(blockCount);
    event.events = EPOLLIN;

    for (int i = 0; i < blockCount; i++) {
        // Append each block's pipe to `epollFD`
        pipe(pipes[i]);
        event.data.u32 = i;
        epoll_ctl(epollFD, EPOLL_CTL_ADD, pipes[i][0], &event);

        // Calculate the max interval and tick size for the timer
        if (blocks[i].interval) {
            maxInterval = MAX(blocks[i].interval, maxInterval);
            timerTick = gcd(blocks[i].interval, timerTick);
        }
    }

    setupSignals();
}

void freeBlocks() {
    for (int i = 0; i < blockCount; i++)
        free(blocks[i].argv);

    free(blocks);
    free(outputs);
    free(blockBuffer);
    free(statusBar[0]);
    free(pipes);
    free(events);
}

int setupBlocks(int nargs, char *args[]) {
    blockCount = nargs >= 3;
    for (int i = 2; i < nargs; i++) {
        if (!strcmp(argsep, args[i])) {
            i += 3;
            if (i < nargs) {
                blockCount++;
            } else {
                fputs("dwmblocks: trailing args:", stderr);
                for (int j = i - 3; j < nargs; j++) {
                    fprintf(stderr, " %s", args[j]);
                }
                fputc('\n', stderr);
                usage();
                return 0;
            }
        }
    }

    if (blockCount == 0) {
        fprintf(stderr, "dwmblocks: No blocks specified\n");
        usage();
        return 0;
    }

    blocks = calloc(blockCount, sizeof(Block));

    outputSize = cmdlength * 4 + clickableBlocks + 1;
    outputs = malloc(blockCount * outputSize * sizeof(char));

    blockBufferSize = outputSize - clickableBlocks;
    blockBuffer = malloc(blockBufferSize * sizeof(*blockBuffer));

    int statusBarSize = blockCount * (outputSize - 1) +
              (blockCount - 1 + leadingDelim) * strlen(delimiter) + 1;
    statusBar[0] = malloc(2 * statusBarSize * sizeof(char));
    statusBar[1] = &statusBar[0][statusBarSize];

    pipes = malloc(blockCount * sizeof(*pipes));
    events = malloc((blockCount + 1) * sizeof(*events));

    if (!blocks || !outputs || !blockBuffer || !statusBar[0] || !pipes || !events) {
        free(blocks);
        free(outputs);
        free(blockBuffer);
        free(statusBar[0]);
        free(pipes);
        free(events);
        fprintf(stderr, "dwmblocks: Alloc failed\n");
        return 0;
    }

    int err = 0;
    int iBlock = 0;
    unsigned int interval = 0;
    unsigned int signal = 0;
    const char **blockArgs;
    for (int i = 0; i + 2 < nargs;) {

        char *endptr;

        errno = 0;
        char *intervalArg = args[i];
        long intervalLong = strtol(args[i], &endptr, 0);
        if (errno != 0 || intervalArg == endptr
            || intervalLong < 0 || intervalLong > UINT_MAX) {
            fprintf(stderr, "dwmblocks: Invalid interval: %s\n", intervalArg);
            err = 1;
            break;
        }

        errno = 0;
        char *signalArg = args[i + 1];
        long signalLong = strtol(signalArg, &endptr, 0);
        if (errno != 0 || signalArg == endptr
            || signalLong < 0 || signalLong > UINT_MAX) {
            fprintf(stderr, "dwmblocks: Invalid signal: %s\n", signalArg);
            err = 1;
            break;
        }

        Block block;
        block.interval = (unsigned int)intervalLong;
        block.signal = (unsigned int)signalLong;

        int j;
        for (j = i + 2; j < nargs; j++) {
            if (!strcmp(argsep, args[j]))
                break;
        }

        int argc = j - (i + 2);
        block.argv = malloc((argc + 1) * sizeof(*block.argv));
        if (!block.argv) {
            fprintf(stderr, "dwmblocks: Alloc failed\n");
            err = 1;
            break;
        }

        memcpy(block.argv, &args[i + 2], argc * sizeof(*block.argv));
        block.argv[argc] = NULL;

        i = j + 1;
        blocks[iBlock++] = block;
    }

    if (err) {
        while (iBlock > 0) {
            free(blocks[--iBlock].argv);
        }
        free(blocks);
        free(outputs);
        free(blockBuffer);
        free(statusBar[0]);
        free(pipes);
        free(events);
        return 0;
    }

    return 1;
}

int main(int argc, char *argv[]) {
    if (setupX()) {
        fprintf(stderr, "dwmblocks: Failed to open display\n");
        return 1;
    }

    int argi;

    writeStatus = setRoot;
    for (argi = 1; argi < argc; argi++) {
        const char *arg = argv[argi];
        if (!strcmp(argsep, arg)) {
            break;
        } else if (!strcmp(CLICKABLE_ARG, arg)) {
            clickableBlocks = 1;
        } else if (!strcmp(LEADING_DELIM_ARG, arg)) {
            leadingDelim = 1;
        } else if (!strncmp(DELIMITER_ARG, arg, LEN(DELIMITER_ARG) - 1)) {
            if (arg[LEN(DELIMITER_ARG) - 1] == '=') {
                delimiter = &arg[LEN(DELIMITER_ARG)];
            } else {
                argi++;
                delimiter = argi < argc ? argv[argi] : "";
            }
        } else if (!strncmp(CMDLENGTH_ARG, arg, LEN(CMDLENGTH_ARG) - 1)) {
            const char *num;
            if (arg[LEN(CMDLENGTH_ARG) - 1] == '=') {
                num = &arg[LEN(CMDLENGTH_ARG)];
            } else {
                argi++;
                if (argi >= argc)
                    break;
                num = argv[argi];
            }

            errno = 0;
            char *endptr;
            long len = strtol(num, &endptr, 0);
            if (errno != 0 || num == endptr || len < 0 || len > CMDLENGTH_MAX) {
                fprintf(stderr, "dwmblocks: invalid cmdlength: %s\n", num);
                return 1;
            }
            cmdlength = (int)len;
        } else if (!strcmp(DEBUG_ARG, arg)) {
            writeStatus = debug;
        } else {
            fprintf(stderr, "invalid argument: %s\n", arg);
            usage();
            return 1;
        }
    }

    if (!setupBlocks(argc - (argi + 1), &argv[argi + 1])) {
        return 1;
    }

    init();
    statusLoop();

    XCloseDisplay(dpy);
    close(epollFD);
    close(signalFD);
    for (int i = 0; i < blockCount; i++) closePipe(pipes[i]);

    freeBlocks();

    return 0;
}
