#include "osm.h"
#include <sys/time.h>

const double SECOND_TO_NANO = 1000000000.0;
const double MICRO_TO_NANO = 1000.0;
#define UNROLL_FACTOR 8
/* calling a system call that does nothing */
#define OSM_NULLSYSCALL asm volatile( "int $0x80 " : : \
        "a" (0xffffffff) /* no such syscall */, "b" (0), "c" (0), "d" (0) /*:\
        "eax", "ebx", "ecx", "edx"*/)


unsigned int round_up_iterations_num(unsigned int iterations) {
    return UNROLL_FACTOR - (iterations % UNROLL_FACTOR) + iterations;
}

/* Time measurement function for a simple arithmetic operation.
   returns time in nano-seconds upon success,
   and -1 upon failure.
   */
double osm_operation_time(unsigned int iterations) {
    if(iterations == 0) {
        return -1;
    }
    struct timeval start, end;
    unsigned int rounded_iterations = round_up_iterations_num(iterations);
    int a,b,c,d,e,f,g,h;
    a=b=c=d=e=f=g=h=0;
    if(gettimeofday(&start, nullptr) == -1) {
        return -1;
    }
    for(unsigned int i = 0; i < rounded_iterations; i += UNROLL_FACTOR) {
        a+=1;
        b+=1;
        c+=1;
        d+=1;
        e+=1;
        f+=1;
        g+=1;
        h+=1;
    }
    if(gettimeofday(&end, nullptr) == -1) {
        return -1;
    }
    double start_elapsed = start.tv_sec * SECOND_TO_NANO + start.tv_usec * MICRO_TO_NANO;
    double end_elapsed = end.tv_sec * SECOND_TO_NANO + end.tv_usec * MICRO_TO_NANO;
    return (end_elapsed-start_elapsed) / double(rounded_iterations);
}

void empty() {

}

/* Time measurement function for an empty function call.
   returns time in nano-seconds upon success,
   and -1 upon failure.
   */
double osm_function_time(unsigned int iterations) {
    if(iterations == 0) {
        return -1;
    }
    struct timeval start, end;
    unsigned int rounded_iterations = round_up_iterations_num(iterations);
    if(gettimeofday(&start, nullptr) == -1) {
        return -1;
    }
    for(unsigned int i = 0; i < rounded_iterations; i += UNROLL_FACTOR) {
        empty();
        empty();
        empty();
        empty();
        empty();
        empty();
        empty();
        empty();
    }
    if(gettimeofday(&end, nullptr) == -1) {
        return -1;
    }
    double start_elapsed = start.tv_sec * SECOND_TO_NANO + start.tv_usec * MICRO_TO_NANO;
    double end_elapsed = end.tv_sec * SECOND_TO_NANO + end.tv_usec * MICRO_TO_NANO;
    return (end_elapsed-start_elapsed) / double(rounded_iterations);
}


/* Time measurement function for an empty trap into the operating system.
   returns time in nano-seconds upon success,
   and -1 upon failure.
   */
double osm_syscall_time(unsigned int iterations) {
    if(iterations == 0) {
        return -1;
    }
    struct timeval start, end;
    unsigned int rounded_iterations = round_up_iterations_num(iterations);
    if(gettimeofday(&start, nullptr) == -1) {
        return -1;
    }
    for(unsigned int i = 0; i < rounded_iterations; i += UNROLL_FACTOR) {
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
        OSM_NULLSYSCALL;
    }
    if(gettimeofday(&end, nullptr) == -1) {
        return -1;
    }
    double start_elapsed = start.tv_sec * SECOND_TO_NANO + start.tv_usec * MICRO_TO_NANO;
    double end_elapsed = end.tv_sec * SECOND_TO_NANO + end.tv_usec * MICRO_TO_NANO;
    return (end_elapsed-start_elapsed) / double(rounded_iterations);
}
