#include "uthreads.h"
#include <iostream>
#include <setjmp.h>
#include <queue>
#include <sys/time.h>
#include <signal.h>


#define SECOND 1000000
#define PREV_JUMP 5

//-----------------------------------------------------------------
#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
            : "=g" (ret)
            : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}


#endif
//--------------------------- Thread class ------------------------------


enum STATE {
    READY, RUNNING, BLOCKED
};

class Thread {
public:
    int tid;
    sigjmp_buf env;
    char *Thread_stack;
    int quantum;
    STATE state;
    int sleep_waiting;
    bool is_sleeping;
    /*
     * class constructor
     */
    Thread(int tid, STATE state, int quantum, thread_entry_point entry_point);
    ~Thread();
};

Thread::Thread(int tid, STATE state, int quantum, thread_entry_point entry_point): tid(tid), quantum(quantum),
                                                                                   state(state){
    Thread_stack = new char[STACK_SIZE];
    sleep_waiting = -1;
    is_sleeping = false;
    address_t sp = (address_t) Thread_stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;
    sigsetjmp(env, 1);
    (env->__jmpbuf)[JB_SP] = translate_address(sp);
    (env->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&env->__saved_mask);
}

Thread::~Thread() {
    delete[] Thread_stack;
}

//--------------------- Helper functions and variables -----------------------

int total_quantum = 0;
sigset_t signal_set; // set of signals that can be blocked
Thread* allThreads[MAX_THREAD_NUM];
std::deque<Thread *> readyThreads;

Thread* runningThread = nullptr;
struct itimerval timer;

void terminateTheProcess() {
    for (auto & thread : allThreads) {
        if(thread != nullptr) {
            delete thread;
        }
    }
}

void blockSignals() {
    if (sigprocmask(SIG_BLOCK, &signal_set, NULL) < 0) {
        std::cerr << "system error: sigprocmask error.\n" << std::endl;
        terminateTheProcess();
        exit(1);
    }
}

void unblockSignals() {
    if (sigprocmask(SIG_UNBLOCK, &signal_set, NULL) < 0) {
        std::cerr << "system error: sigprocmask error.\n" << std::endl;
        terminateTheProcess();
        exit(1);
    }
}

/*
 * finding the smallest id.
 */
int find_tid() {
    for (int i = 1; i < MAX_THREAD_NUM; i++) {
        if (allThreads[i] == nullptr) {
            return i;
        }
    }
    return -1;
}

/*
 * check if tid is valid.
 */
int check_tid(int tid) {
    if (tid < 0 || tid >= MAX_THREAD_NUM) {
        std::cerr << "thread library error: invalid tid." << std::endl;
        return -1;
    }
    return 0;
}

void resetTimer() {
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL)) {
        std::cerr << "system error: setitimer error during init.\n" << std::endl;
        terminateTheProcess();
        exit(1);
    }
}

/*
 * function that check if sleeping thread needs to wake up.
 * It also updates sleeping time.
 */
void checkWaiting() {
    for (auto &thread : allThreads) {
        if (thread != nullptr) {
            if (thread->is_sleeping) {
                thread->sleep_waiting--;
            }
            if (thread->sleep_waiting == 0) {
                thread->is_sleeping = false;
                if (thread->state == READY) {
                    // if we didn't get here, this means that the thread was blocked and also sleeping.
                    readyThreads.push_back(thread);
                }
                thread->sleep_waiting = -1;
            }
        }
    }
}

/*
 * gets next thread.
 */
void nextThread() {
    // get next thread to run
    runningThread = readyThreads.front();
    readyThreads.pop_front();

    runningThread->state = RUNNING;
    runningThread->quantum++;
    total_quantum++;
}

/**
 * handles SIGVTALRM Signal.
 * @param sig
 */
void handler(int sig) {
    // block signals in signal_set (SIGVTALRM)
    blockSignals();
    // check sleeping threads.
    checkWaiting();

    int ret_val = sigsetjmp(runningThread->env, 1);
    // if we have arrived here after a jump (siglongjmp), return from the function
    if (ret_val == PREV_JUMP) {
        return;
    }

    // set READY.
    runningThread->state = READY;
    readyThreads.push_back(runningThread);

    // get next thread.
    nextThread();

    resetTimer();
    unblockSignals();
    siglongjmp(runningThread->env, PREV_JUMP);
}

//-----------------------------------------------------------------------

/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs) {
    if(quantum_usecs <= 0 ) {
        std::cerr << "thread library error: quantum_usecs must be positive." << std::endl;
        return -1;
    }
    for (int i = 0; i < MAX_THREAD_NUM; i++) {
        allThreads[i] = nullptr;
    }
    total_quantum++;

    // Install timer_handler as the signal handler for SIGVTALRM.
    struct sigaction sa = {0};
    sa.sa_handler = &handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0) {
        std::cerr << "system error: sigaction error during init.\n" << std::endl;
        exit(1);
    }

    // initialise main thread
    Thread *mainThread = new Thread(0, RUNNING, 1, nullptr);
    allThreads[0] = mainThread;
    runningThread = mainThread;

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGVTALRM);

    // Configure the timer
    timer.it_value.tv_sec = quantum_usecs / SECOND; // first time interval, seconds part
    timer.it_value.tv_usec = quantum_usecs % SECOND; // first time interval, microseconds part

    timer.it_interval.tv_sec = quantum_usecs / SECOND; // following time intervals, seconds part
    timer.it_interval.tv_usec = quantum_usecs % SECOND; // following time intervals, microseconds part
    // Start a virtual timer. It counts down whenever this process is executing.
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL)) {
        std::cerr << "system error: setitimer error during init.\n" << std::endl;
        delete mainThread;
        exit(1);
    }
    return 0;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point) {
    blockSignals();
    if (entry_point == nullptr) {
        std::cerr << "thread library error: entry_point is null." << std::endl;
        unblockSignals();
        return -1;
    }
    int tid = find_tid();
    if (tid == -1) {
        std::cerr << "thread library error: Exceeded MAX_THREAD_NUM." << std::endl;
        unblockSignals();
        return -1;
    }
    Thread *new_thread = new Thread(tid, READY, 0, entry_point);
    allThreads[tid] = new_thread;
    readyThreads.push_back(new_thread);
    unblockSignals();
    return tid;
}


/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid) {
    blockSignals();
    // check if tid is valid.
    if (check_tid(tid) == -1) {
        unblockSignals();
        return -1;
    }
    // if valid, check if present.
    if (allThreads[tid] == nullptr) {
        std::cerr << "thread library error: Thread tid is not present." << std::endl;
        unblockSignals();
        return -1;
    }
    // check if tid of main thread:
    if (tid == 0) {
        terminateTheProcess();
        exit(0);
    }
    // tid is the currently running thread:
    if (runningThread->tid == tid) {
        delete allThreads[tid];
        allThreads[tid] = nullptr;
        checkWaiting();
        nextThread();
        resetTimer();
        unblockSignals();
        siglongjmp(runningThread->env, PREV_JUMP);
    }
    else {
        // other (non running) thread is terminated.
        // if in ready, remove it:
        for (unsigned int i = 0; i < readyThreads.size(); i++) {
            if (readyThreads[i] == allThreads[tid]) {
                readyThreads.erase(readyThreads.begin() + i);
            }
        }
        delete allThreads[tid];
        allThreads[tid] = nullptr;
    }
    unblockSignals();
    return 0;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid) {
    blockSignals();
    // check if tid is valid.
    if (check_tid(tid) == -1) {
        unblockSignals();
        return -1;
    }
    // if valid, check if present.
    if (allThreads[tid] == nullptr) {
        std::cerr << "thread library error: Thread tid is not present." << std::endl;
        unblockSignals();
        return -1;
    }
    if (tid == 0) {
        std::cerr << "thread library error: Cannot block main Thread." << std::endl;
        unblockSignals();
        return -1;
    }
    // if the thread we want to block is sleeping.
    if (allThreads[tid]->is_sleeping) {
        allThreads[tid]->state = BLOCKED;
    }
    // if thread already blocked, do nothing.
    else if (allThreads[tid]->state == BLOCKED) {
        unblockSignals();
        return 0;
    }
    else {
        // if we want to block running thread.
        if (runningThread->tid == tid) {
            allThreads[tid]->state = BLOCKED;
            checkWaiting();
            int ret_val = sigsetjmp(runningThread->env, 1);
            // if we have arrived here after a jump (siglongjmp), return from the function
            if (ret_val == PREV_JUMP) {
                unblockSignals();
                return 0;
            }
            nextThread();
            resetTimer();
            unblockSignals();
            siglongjmp(runningThread->env, PREV_JUMP);
        }
        else {
            // other (non running) thread is blocked.
            // if in ready, remove it:
            allThreads[tid]->state = BLOCKED;
            for (unsigned int i = 0; i < readyThreads.size(); i++) {
                if (readyThreads[i] == allThreads[tid]) {
                    readyThreads.erase(readyThreads.begin() + i);
                }
            }
        }
    }
    unblockSignals();
    return 0;
}


/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid) {
    blockSignals();
    // check if tid is valid.
    if (check_tid(tid) == -1) {
        unblockSignals();
        return -1;
    }
    // if valid, check if present.
    if (allThreads[tid] == nullptr) {
        std::cerr << "thread library error: Thread tid is not present." << std::endl;
        unblockSignals();
        return -1;
    }

    // if thread is RUNNING or READY, return.
    if (uthread_get_tid() == tid || allThreads[tid]->state == READY) {
        unblockSignals();
        return 0;
    }

    if (allThreads[tid]->is_sleeping) {
        // stay sleeping, but become READY exactly after sleeping_time ends.
        allThreads[tid]->state = READY;
    }
    // if only BLOCKED.
    else {
        allThreads[tid]->state = READY;
        // add to readyThreads
        readyThreads.push_back(allThreads[tid]);
    }
    unblockSignals();
    return 0;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums) {
    blockSignals();
    if (runningThread->tid == 0) {
        std::cerr << "thread library error: It is illegal to sleep the Main thread." << std::endl;
        unblockSignals();
        return -1;
    }
    if (num_quantums <= 0) {
        std::cerr << "thread library error: num_quantums should be positive." << std::endl;
        unblockSignals();
        return -1;
    }
    checkWaiting();
    int ret_val = sigsetjmp(runningThread->env, 1);
    if (ret_val == PREV_JUMP) {
        unblockSignals();
        return 0;
    }
    // label runningThread to be READY when the sleeping_wait finish.
    runningThread->state = READY;
    runningThread->is_sleeping = true;
    runningThread->sleep_waiting = num_quantums;
    nextThread();
    resetTimer();
    unblockSignals();
    siglongjmp(runningThread->env, PREV_JUMP);
    return 0;
}


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid() {
    return runningThread->tid;
}


/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums() {
    return total_quantum;
}


/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid) {
    blockSignals();
    // check if tid is valid.
    if (check_tid(tid) == -1) {
        unblockSignals();
        return -1;
    }
    // if valid, check if present.
    if (allThreads[tid] == nullptr) {
        unblockSignals();
        std::cerr << "thread library error: Thread tid is not present." << std::endl;
        return -1;
    }
    unblockSignals();
    return allThreads[tid]->quantum;
}
