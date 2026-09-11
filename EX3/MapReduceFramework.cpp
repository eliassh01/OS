#include "MapReduceFramework.h"
#include <pthread.h>
#include <atomic>
#include <iostream>
#include <algorithm>

struct ThreadContext;

// -------------- Barrier clas -----------------
class Barrier {
public:
    Barrier(int numThreads);
    ~Barrier();
    void barrier();

private:
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    int count;
    int numThreads;
};

Barrier::Barrier(int numThreads)
        : mutex(PTHREAD_MUTEX_INITIALIZER)
        , cv(PTHREAD_COND_INITIALIZER)
        , count(0)
        , numThreads(numThreads)
{ }


Barrier::~Barrier()
{
    if (pthread_mutex_destroy(&mutex) != 0) {
        fprintf(stderr, "[[Barrier]] error on pthread_mutex_destroy");
        exit(1);
    }
    if (pthread_cond_destroy(&cv) != 0){
        fprintf(stderr, "[[Barrier]] error on pthread_cond_destroy");
        exit(1);
    }
}


void Barrier::barrier()
{
    if (pthread_mutex_lock(&mutex) != 0){
        fprintf(stderr, "[[Barrier]] error on pthread_mutex_lock");
        exit(1);
    }
    if (++count < numThreads) {
        if (pthread_cond_wait(&cv, &mutex) != 0){
            fprintf(stderr, "[[Barrier]] error on pthread_cond_wait");
            exit(1);
        }
    } else {
        count = 0;
        if (pthread_cond_broadcast(&cv) != 0) {
            fprintf(stderr, "[[Barrier]] error on pthread_cond_broadcast");
            exit(1);
        }
    }
    if (pthread_mutex_unlock(&mutex) != 0) {
        fprintf(stderr, "[[Barrier]] error on pthread_mutex_unlock");
        exit(1);
    }
}

// ------------------------------------------

typedef struct JobContext {
    const MapReduceClient* client;
    const InputVec* inputVec;
    OutputVec* outputVec;
    int threadNum;
    ThreadContext* threadContexts;

    // boolean variable
    bool joined;
    bool didntStartReducePhase;

    // atomic variables
    std::atomic<unsigned int> mapPhaseOldValuesDist;
    // number of (K2, V2 ) pairs produced in map phase.
    std::atomic<unsigned int> mapPhaseIntermediaryPairsCounter;
    // counts num of reads to map func.
    std::atomic<unsigned int> mapPhaseNumOfProcessedElem;
    // counts number of pairs shuffled until now.
    std::atomic<unsigned int> shufflePhaseNumOfProcessedElem;

    std::atomic<unsigned int> reducePhaseOldValuesDist;
    // counts num of reads to reduce func.
    std::atomic<unsigned int> reducePhaseNumOfProcessedElem;

    JobState job_state;

    // mutexes
    pthread_mutex_t stateMutex;
    pthread_mutex_t emit2Mutex;
    pthread_mutex_t emit3Mutex;

    // shuffle vector.
    std::vector<IntermediateVec*> shuffledVector;

}JobContext;


typedef struct ThreadContext {
    pthread_t thread;
    // list where we save the outputs of the mapPhase for each thread.
    IntermediateVec* mapIntermediateVector;
    JobContext* job_context;
    Barrier *barrier{};
    bool shuffleThread = false;
}ThreadContext;



void mutexLock(pthread_mutex_t* mutex, const std::string& errMsg) {
    if (pthread_mutex_lock(mutex) != 0) {
        std::cout << errMsg << std::endl;
        exit(1);
    }
}

void mutexUnlock(pthread_mutex_t* mutex, const std::string& errMsg) {
    if (pthread_mutex_unlock(mutex) != 0) {
        std::cout << errMsg << std::endl;
        exit(1);
    }
}

void mapPhase(ThreadContext* thread_context) {
    bool mappingFlag = true;
    while (mappingFlag) {
        unsigned int old_value = thread_context->job_context->mapPhaseOldValuesDist++;
        if (old_value < thread_context->job_context->inputVec->size()) {
            K1* key1 = thread_context->job_context->inputVec->at(old_value).first;
            V1* value1 = thread_context->job_context->inputVec->at(old_value).second;
            thread_context->job_context->client->map(key1, value1, thread_context);
            thread_context->job_context->mapPhaseNumOfProcessedElem++;
            mutexLock(&thread_context->job_context->stateMutex,
                      "system error: stateMutex lock failed");
            float percentage = ((float) thread_context->job_context->mapPhaseNumOfProcessedElem /
                                 (float) thread_context->job_context->inputVec->size()) * 100;
            thread_context->job_context->job_state.percentage = percentage;
            mutexUnlock(&thread_context->job_context->stateMutex,
                        "system error: stateMutex unlock failed");
        }
        else {
            mappingFlag = false;
        }
    }
}

bool sortByKey(IntermediatePair &lhs, IntermediatePair &rhs) {
    return (*lhs.first) < (*rhs.first);
}

void sortPhase(ThreadContext* thread) {
    std::sort(thread->mapIntermediateVector->begin(), thread->mapIntermediateVector->end(), sortByKey);
}

void shufflePhase(ThreadContext* thread_context) {
    bool stillPairsToProcess = true;

    int counter = 0;
    // (K2*, V2*) pair.
    IntermediatePair maxPair;

    while (stillPairsToProcess) {
        while (counter < thread_context->job_context->threadNum) {
            // finding the first non-empty intermediate vector, and then taking the pair in its back.
            if (!thread_context->job_context->threadContexts[counter].mapIntermediateVector->empty()) {
                maxPair = thread_context->job_context->threadContexts[counter].mapIntermediateVector->back();
                if (maxPair.first == nullptr) {
                    continue;
                }
                break;
            }
            counter++;
        }

        counter = 0;
        // finding the pair with the max key.
        while (counter < thread_context->job_context->threadNum) {
            if (!thread_context->job_context->threadContexts[counter].mapIntermediateVector->empty() &&
                thread_context->job_context->threadContexts[counter].mapIntermediateVector->back().first != nullptr &&
                (*maxPair.first) <
                (*thread_context->job_context->threadContexts[counter].mapIntermediateVector->back().first)) {
                maxPair = thread_context->job_context->threadContexts[counter].mapIntermediateVector->back();
            }
            counter++;
        }

        counter = 0;
        // define the new vector which contains all the pairs with the max key found.
        IntermediateVec* maxKeyVector = new IntermediateVec;
        IntermediatePair curPair;
        for (int i = 0; i < thread_context->job_context->threadNum; i++) {
            if (thread_context->job_context->threadContexts[i].mapIntermediateVector->empty()) {
                continue;
            }
            curPair = thread_context->job_context->threadContexts[i].mapIntermediateVector->back();
            while(curPair.first != nullptr &&
                  !((*maxPair.first) < *curPair.first) && !(*curPair.first < (*maxPair.first))) {
                thread_context->job_context->threadContexts[i].mapIntermediateVector->pop_back();
                maxKeyVector->push_back(curPair);
                thread_context->job_context->shufflePhaseNumOfProcessedElem++;
                mutexLock(&thread_context->job_context->stateMutex,
                          "system error: stateMutex lock failed");
                float percentage = ((float) thread_context->job_context->shufflePhaseNumOfProcessedElem /
                                     (float) thread_context->job_context->mapPhaseIntermediaryPairsCounter) * 100;
                thread_context->job_context->job_state.percentage = percentage;
                mutexUnlock(&thread_context->job_context->stateMutex,
                            "system error: stateMutex unlock failed");
                if (!thread_context->job_context->threadContexts[i].mapIntermediateVector->empty()) {
                    curPair = thread_context->job_context->threadContexts[i].mapIntermediateVector->back();
                }
                else {
                    break;
                }
            }
        }

        if (!maxKeyVector->empty()) {
            thread_context->job_context->shuffledVector.push_back(maxKeyVector);
        }

        // check if intermediaryVector of each thread is empty, if yes we are done.
        int numThreadsEmpty = 0;
        for (int i = 0; i < thread_context->job_context->threadNum; i++) {
            if (thread_context->job_context->threadContexts[i].mapIntermediateVector->empty()) {
                numThreadsEmpty++;
            }
        }
        if (numThreadsEmpty == thread_context->job_context->threadNum) {
            stillPairsToProcess = false;
        }
    }
}

void reducePhase(ThreadContext* thread_context) {
    bool reduceFlag = true;
    while (reduceFlag) {
        unsigned int old_value = thread_context->job_context->reducePhaseOldValuesDist++;
        if (old_value < thread_context->job_context->shuffledVector.size()) {
            thread_context->job_context->client->reduce(
                    thread_context->job_context->shuffledVector.at(old_value), thread_context);
            thread_context->job_context->reducePhaseNumOfProcessedElem++;
            mutexLock(&thread_context->job_context->stateMutex,
                      "system error: stateMutex lock failed");
            float percentage = ((float) thread_context->job_context->reducePhaseNumOfProcessedElem /
                                (float) thread_context->job_context->shuffledVector.size()) * 100;
            thread_context->job_context->job_state.percentage = percentage;
            mutexUnlock(&thread_context->job_context->stateMutex,
                        "system error: stateMutex unlock failed");
        }
        else {
            reduceFlag = false;
        }
    }
}

void* JobsManagement(void *context) {
    ThreadContext* thread_context = static_cast<ThreadContext*> (context);
    mutexLock(&thread_context->job_context->stateMutex,
              "system error: stateMutex lock failed");
    thread_context->job_context->job_state.stage = MAP_STAGE;
    mutexUnlock(&thread_context->job_context->stateMutex,
                "system error: stateMutex unlock failed");

    mapPhase(thread_context);
    sortPhase(thread_context);

    thread_context->barrier->barrier();

    if (thread_context->shuffleThread) {
        // change state to shuffle.
        mutexLock(&thread_context->job_context->stateMutex,
                  "system error: stateMutex lock failed");
        thread_context->job_context->job_state.stage = SHUFFLE_STAGE;
        thread_context->job_context->job_state.percentage = 0;
        mutexUnlock(&thread_context->job_context->stateMutex,
                    "system error: stateMutex unlock failed");
        shufflePhase(thread_context);
    }

    thread_context->barrier->barrier();

    mutexLock(&thread_context->job_context->stateMutex,
              "system error: stateMutex lock failed");
    if (thread_context->job_context->didntStartReducePhase) {
        thread_context->job_context->job_state.stage = REDUCE_STAGE;
        thread_context->job_context->job_state.percentage = 0;
        thread_context->job_context->didntStartReducePhase = false;
    }
    mutexUnlock(&thread_context->job_context->stateMutex,
                "system error: stateMutex unlock failed");

    reducePhase(thread_context);
    return nullptr;
}


void emit2 (K2* key, V2* value, void* context) {
    ThreadContext* thread_context = static_cast<ThreadContext*> (context);
    thread_context->job_context->mapPhaseIntermediaryPairsCounter++;

    mutexLock(&thread_context->job_context->emit2Mutex, "system error: emit2Mutex lock failed");
    thread_context->mapIntermediateVector->push_back(std::pair<K2*, V2*> (key, value));
    mutexUnlock(&thread_context->job_context->emit2Mutex, "system error: emit2Mutex unblock failed");
}

void emit3 (K3* key, V3* value, void* context) {
    ThreadContext* thread_context = static_cast<ThreadContext*> (context);

    mutexLock(&thread_context->job_context->emit3Mutex, "system error: emit3Mutex lock failed");
    thread_context->job_context->outputVec->push_back(std::pair<K3*, V3*> (key, value));
    mutexUnlock(&thread_context->job_context->emit3Mutex, "system error: emit3Mutex unblock failed");
}


/*
 * This function starts running the MapReduce algorithm (with several threads) and returns a JobHandle.
 */
JobHandle startMapReduceJob(const MapReduceClient& client,
                            const InputVec& inputVec, OutputVec& outputVec,
                            int multiThreadLevel) {
    JobContext* job_context = new JobContext();
    job_context->client = &client;
    job_context->inputVec = &inputVec;
    job_context->outputVec = &outputVec;
    job_context->threadNum = multiThreadLevel;
    job_context->didntStartReducePhase = true;


    // init mutexes.
    job_context->stateMutex = PTHREAD_MUTEX_INITIALIZER;
    job_context->emit2Mutex = PTHREAD_MUTEX_INITIALIZER;
    job_context->emit3Mutex = PTHREAD_MUTEX_INITIALIZER;

    // initialise job_stage, and its mutex.
    job_context->job_state.stage = UNDEFINED_STAGE;
    job_context->job_state.percentage = 0;

    // initialise atomic counter.
    job_context->mapPhaseOldValuesDist = 0;
    job_context->mapPhaseNumOfProcessedElem = 0;
    job_context->mapPhaseIntermediaryPairsCounter = 0;
    job_context->shufflePhaseNumOfProcessedElem = 0;
    job_context->reducePhaseOldValuesDist = 0;
    job_context->reducePhaseNumOfProcessedElem = 0;

    job_context->threadContexts = new ThreadContext[multiThreadLevel];
    if (job_context->threadContexts == nullptr) {
        std::cout << "system error: allocation error." << std::endl;
        exit(1);
    }

    // Barrier init.
    Barrier* barrier = new Barrier(multiThreadLevel);

    job_context->threadContexts[0].shuffleThread = true;

    // initialisation of ThreadContexts list.
    for (int i = 0; i < multiThreadLevel; i++) {
        job_context->threadContexts[i].job_context = job_context;
        job_context->threadContexts[i].barrier = barrier;
        if (job_context->threadContexts[i].barrier == nullptr) {
            std::cout << "system error: allocation error." << std::endl;
            exit(1);
        }
        job_context->threadContexts[i].mapIntermediateVector = new IntermediateVec;
        if (job_context->threadContexts[i].mapIntermediateVector == nullptr) {
            std::cout << "system error: allocation error." << std::endl;
            exit(1);
        }
    }

    // creating threads.
    for (int i = 0; i < multiThreadLevel; i++){
        if (pthread_create(&job_context->threadContexts[i].thread, NULL,
                       JobsManagement, &job_context->threadContexts[i]) != 0) {
            std::cout << "system error: Thread creation failed." << std::endl;
            exit(1);
        }
    }
    return static_cast<JobHandle> (job_context);
}

void waitForJob(JobHandle job) {
    JobContext* job_context = static_cast<JobContext*> (job);
    if (job_context->joined) {
        return;
    }
    for (int i = 0; i < job_context->threadNum; i++) {
        if (pthread_join(job_context->threadContexts[i].thread, nullptr) != 0) {
            std::cout << "system error: pthread_joined failed." << std::endl;
            exit(1);
        }
    }
    job_context->joined = true;
}

void getJobState(JobHandle job, JobState* state) {
    JobContext* job_context = static_cast<JobContext*> (job);
    mutexLock(&job_context->stateMutex,
              "system error: stateMutex lock failed");
    state->stage = job_context->job_state.stage;
    state->percentage = job_context->job_state.percentage;
    mutexUnlock(&job_context->stateMutex,
                "system error: stateMutex unlock failed");
}


void closeJobHandle(JobHandle job) {
    waitForJob(job);
    JobContext* job_context = static_cast<JobContext*> (job);
    if (pthread_mutex_destroy(&job_context->stateMutex) != 0) {
        std::cout << "system error: destroying stateMutex failed." << std::endl;
        exit(1);
    }
    if (pthread_mutex_destroy(&job_context->emit2Mutex) != 0) {
        std::cout << "system error: destroying emit2Mutex failed." << std::endl;
        exit(1);
    }
    if (pthread_mutex_destroy(&job_context->emit3Mutex) != 0) {
        std::cout << "system error: destroying jobManagementMutex failed." << std::endl;
        exit(1);
    }
    delete job_context->threadContexts[0].barrier;
    for (int i = 0; i < job_context->threadNum; i++) {
        delete job_context->threadContexts[i].mapIntermediateVector;
    }
    for (auto & vector : job_context->shuffledVector) {
        delete vector;
    }
    delete[] job_context->threadContexts;
    delete job_context;
}

