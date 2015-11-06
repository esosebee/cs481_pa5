#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/resource.h>
// INT_MAX in limits.h, "const int INT_MAX = 0x7FFFFFFF;"
typedef enum {true=1, false=0} Bool;
#define MAXQLEN 200
#define WORKTHREADNUM threadCount
#define THREADSTACK  65536
int constPriority; //controls the case or priority from the specs
//These variables will help us keep track of whose turn it is for part 2c
int reader_turn = 1;
int writer_turn = 1;
//****************************************************************
// Math function to produce poisson distribution based on mean
//****************************************************************
int RandPoisson(double mean) {
    double limit = exp(-mean), product = ((double)rand()/INT_MAX); 
    int count=0;
    for (; product > limit; count++) 
        product *= ((double)rand()/INT_MAX);
    return count;
}

//****************************************************************
// Generic person record, for Reader(0)/Writer(1)
typedef struct {int arrvT, deptT, pID, RWtype;} P437;

//****************************************************************
// bookkeeping 
typedef struct {volatile int numDeny,numR,numW,sumRwait,sumWwait,maxRwait,maxWwait,roomRmax;} Data437;
Data437 data;
// Global shared data among all threads
volatile int gbRcnt = 0, gbWcnt=0, gbRwait=0, gbWwait=0, gbRnum=0, gbWnum=0;
volatile int gbID = 0, gbVClk=0, gbRoomBusy = false;

//****************************************************************
// Queue handing
typedef struct {P437 *entry[MAXQLEN]; int front, rear, len; pthread_mutex_t L;} Q437;
Q437 RreqQ, WreqQ, CombinedQ;

typedef struct {
    pthread_mutex_t m; /* read/write monitor lock */
    int rwlock;
    /* >0=# rdrs, <0=wrtr, 0=none */
    pthread_cond_t readers_ok; /* start waiting readers */
    unsigned int waiting_writers; /* # of waiting writers */
    pthread_cond_t writer_ok; /* start a waiting writer */
} rwl_t;

void QueueInit(Q437 *ptrQ) { // the Queue is initialized to be empty
    ptrQ->front = ptrQ->len = 0;
    ptrQ->rear = MAXQLEN-1;     // circular Q, FIFO
    pthread_mutex_init(&ptrQ->L,NULL);
}
Bool QueueEmpty(Q437 *ptrQ) {return (ptrQ->len==0) ? true : false; }
Bool QueueFull(Q437 *ptrQ)  {return (ptrQ->len>=MAXQLEN) ? true : false; }
int  QueueSize(Q437 *ptrQ) {return ptrQ->len; }

P437* QueueAppend(Q437 *ptrQ, P437 *ptrEntry) {
    // if the Queue is full return overflow else item is appended to the queue
    pthread_mutex_lock(&ptrQ->L); //protect Q
    if (ptrQ->len<MAXQLEN) { 
        ptrQ->len++; ptrQ->rear = (ptrQ->rear+1)%MAXQLEN;
        ptrQ->entry[ptrQ->rear] = ptrEntry;
    }
    pthread_mutex_unlock(&ptrQ->L);
    return ptrEntry;
}

P437* QueuePop(Q437 *ptrQ) {
    // Post: if the Queue is not empty the front of the Queue is removed/returned
    P437 *ptrEntry = NULL;
    pthread_mutex_lock(&ptrQ->L);
    if (ptrQ->len > 0) {
        ptrEntry = ptrQ->entry[ptrQ->front];
        ptrQ->entry[ptrQ->front] = NULL;
        ptrQ->len--;
        ptrQ->front = (ptrQ->front+1)%MAXQLEN;
    }
    pthread_mutex_unlock(&ptrQ->L);
    return ptrEntry;
}

P437* QueueTop(Q437 *ptrQ) {
    // Post: if the Queue is not empty the front of the Queue is returned
    if (ptrQ->len == 0) return NULL;
    else return(ptrQ->entry[ptrQ->front]);
}

void
rwl_init(rwl_t *rwlp)
{
    pthread_mutex_init(&rwlp->m, NULL);
    pthread_cond_init(&rwlp->readers_ok,  NULL);
    pthread_cond_init(&rwlp->writer_ok,  NULL);
    rwlp->rwlock = 0;
    rwlp->waiting_writers = 0;
}
/*
 * Acquire a read lock.
 */
void
rwl_rdlock(rwl_t *rwlp)
{
    pthread_mutex_lock(&rwlp->m);
    while (rwlp->rwlock < 0)
    {
        gbRwait++; // Increment the number of waiting readers in the queue
        pthread_cond_wait(&rwlp->readers_ok, &rwlp->m);
        gbRwait--; // Decrement the number of waiting readers in the queue
    }

    // Set room to busy and increment the number of readers in the room
    gbRoomBusy = 1; gbRcnt++;
    rwlp->rwlock++;
    pthread_mutex_unlock(&rwlp->m);
}
/*
 * Acquire a read lock and checks for room capacity. If the capacity
 * of the room is met, then readers will wait.
 */
void
countingRLock(rwl_t *rwlp)
{
    pthread_mutex_lock(&rwlp->m);
    while (rwlp->rwlock < 0 || gbRcnt == data.roomRmax)
    {
        gbRwait++; // Increment the number of waiting readers in the queue
        pthread_cond_wait(&rwlp->readers_ok, &rwlp->m);
        gbRwait--; // Decrement the number of waiting readers in the queue
    }

    // Set room to busy and increment the number of readers in the room
    gbRoomBusy = 1; gbRcnt++;
    rwlp->rwlock++;
    pthread_mutex_unlock(&rwlp->m);
}
/*
 * Acquire a read lock. In our final implementation 2d (Case C), we alternate our
 * waiting queue. For example, if the room is filled with one or more readers and
 * we have a writer next in line in the waiting queue, then all other readers will
 * wait until the writer is able to fill the room.
 */
void
rwl_rdlock3(rwl_t *rwlp)
{
    pthread_mutex_lock(&rwlp->m);
    P437 *newptr;
    if (QueueEmpty(&CombinedQ) == false)
    {
        newptr = QueueTop(&CombinedQ);
    }
    //While the room is not locked by a reader and it's the not the readers turn
    // readers will continue to wait
    while (rwlp->rwlock < 0 || (newptr != NULL && newptr->RWtype == 1))
    {
        gbRwait++; // Increment the number of waiting readers in the queue
        pthread_cond_wait(&rwlp->readers_ok, &rwlp->m);
        gbRwait--; // Decrement the number of waiting readers in the queue
    }

    // Set room to busy and increment the number of readers in the room
    QueuePop(&CombinedQ);
    gbRoomBusy = 1; gbRcnt++;
    rwlp->rwlock++;
    pthread_mutex_unlock(&rwlp->m);
}
/*
 * Acquire a write lock. In our final implementation 2d (Case C), we alternate our
 * waiting queue. For example, if the room is filled with one or more readers and
 * we have a writer next in line in the waiting queue, then all other readers will
 * wait until the writer is able to fill the room.
 */
void
rwl_wrlock(rwl_t *rwlp)
{
    pthread_mutex_lock(&rwlp->m);
    while (rwlp->rwlock != 0 || gbRwait != 0) {
        rwlp->waiting_writers++;
        gbWwait++;  // Increment the number of writers waiting in the queue
        pthread_cond_wait(&rwlp->writer_ok, &rwlp->m);
        rwlp->waiting_writers--;
        gbWwait--; // Decrement the number of writers in the queue
    }

    // Set room to busy for writer and increment the number of writers in room
    gbRoomBusy = 1;
    gbWcnt++;
    rwlp->rwlock = -1;
    pthread_mutex_unlock(&rwlp->m);
}
/*
 * Acquire a write lock. Only a single writer can proceed. Because 2b (Case A)
 * and 2c (Case B) prioritizes readers, we check if there are any readers waiting.
 * If there are, writers will continue to wait.
 */
void
rwl_wrlock3(rwl_t *rwlp)
{
    pthread_mutex_lock(&rwlp->m);
    P437 *newptr;
    if (QueueEmpty(&CombinedQ) == false)
    {
        newptr = QueueTop(&CombinedQ);
    }
    while (rwlp->rwlock != 0 || (newptr != NULL && newptr->RWtype == 0)) {
        rwlp->waiting_writers++;
        gbWwait++;  // Increment the number of writers waiting in the queue
        pthread_cond_wait(&rwlp->writer_ok, &rwlp->m);
        rwlp->waiting_writers--;
        gbWwait--; // Decrement the number of writers in the queue
    }

    // Set room to busy for writer and increment the number of writers in room
    QueuePop(&CombinedQ);
    gbRoomBusy = 1;
    gbWcnt++;
    rwlp->rwlock = -1;
    pthread_mutex_unlock(&rwlp->m);
}
/*
 * Unlock the read/write lock.
 */
void
rwl_unlock(rwl_t *rwlp)
{
    int ww, wr;

    pthread_mutex_lock(&rwlp->m);
    if (rwlp->rwlock < 0) /* rwlock < 0 if locked for writing */
    {
        rwlp->rwlock = 0;
        gbWcnt--;
        gbRoomBusy = 0;
    }
    else
    {
        rwlp->rwlock--;
        gbRcnt--;
        if (gbRcnt == 0) gbRoomBusy = 0;
    }
    /*
     * Keep flags that show if there are waiting readers or writers so
     * that we can wake them up outside the monitor lock.
     */
    ww = (rwlp->waiting_writers && rwlp->rwlock == 0);
    wr = (rwlp->waiting_writers == 0);
    pthread_mutex_unlock(&rwlp->m);


    if (constPriority == 3)
    {
        P437 *newptr;
        if (QueueEmpty(&CombinedQ) == false)
        {
            newptr = QueueTop(&CombinedQ);

            if (newptr->RWtype == 1)
                pthread_cond_signal(&rwlp->writer_ok);
            else if (newptr->RWtype == 0)
                pthread_cond_broadcast(&rwlp->readers_ok);
        }
        else
        {
                pthread_cond_broadcast(&rwlp->readers_ok);
                pthread_cond_signal(&rwlp->writer_ok);
        }
    }
    else
    {
        //Prioritizes readers for 2b (Case A) and 2c (Case B)
        if (gbRwait)
            pthread_cond_broadcast(&rwlp->readers_ok);
        else if (gbWwait)
            pthread_cond_signal(&rwlp->writer_ok);
    }
}

// option parameter, set once
//int constT2read=10; //spend 10s to read
int constT2read; //spend 10s to read
int constT2write; //spend 1s to write
//int timers=60*60; // default one hour, real time about 4-10 secs
int timers;
//double meanR=10.0, meanW = 2.0, gbTstart;
double meanR, meanW, gbTstart;

// Used to specify random seed
//int seed=1;
int seed;

//int threadCount = 17;
int threadCount;

static pthread_mutex_t gbLock = PTHREAD_MUTEX_INITIALIZER;
//static pthread_mutex_t gbRLock = PTHREAD_MUTEX_INITIALIZER;
//static pthread_mutex_t gbWLock = PTHREAD_MUTEX_INITIALIZER;
//static pthread_cond_t gbRoomSem = PTHREAD_COND_INITIALIZER;

rwl_t myLock, gbRLock, gbWLock, gbRoomSem;



//****************************************************************
// Virtual Clock for simulation (in seconds)
//****************************************************************
long InitTime() { 
    struct timeval st;
    gettimeofday(&st, NULL);
    return(gbTstart = (1000.0*st.tv_sec+st.tv_usec/1000.0));
}
long GetTime() { // real wall clock in milliseconds 
    struct timeval st;
    gettimeofday(&st, NULL);
    return (1000.0*st.tv_sec+st.tv_usec/1000.0-gbTstart);
}
void Sleep437(long usec) { // sleep in microsec
    struct timespec tim, tim2;
    tim.tv_sec = usec/1000000L;
    tim.tv_nsec = (usec-tim.tv_sec*1000000L)*1000;
    nanosleep(&tim,&tim2);
}

//****************************************************************
// Routines to process Read/Write
//****************************************************************
void EnterReader0(P437 *ptr, int threadid) {
    // Leaving the Room
    pthread_mutex_lock(&gbLock);
    gbRoomBusy = 1; gbRcnt=1;
    if (gbRcnt>data.roomRmax) data.roomRmax = gbRcnt;
}

void EnterReader1(P437 *ptr, int threadid) {
    // try to Enter the room
    rwl_rdlock(&myLock);
    if (gbRcnt>data.roomRmax) data.roomRmax = gbRcnt;
}

void EnterReader2(P437 *ptr, int threadid) {
    // try to Enter the room
    countingRLock(&myLock);
}

void EnterReader3(P437 *ptr, int threadid) {
    // try to Enter the room
    rwl_rdlock3(&myLock);
}

void DoReader(P437 *ptr, int threadid) {
    int wT;
    // Reading
    ptr->deptT = gbVClk;
    wT = ptr->deptT - ptr->arrvT;
    if (wT>data.maxRwait) data.maxRwait=wT; 
    data.numR++; data.sumRwait += wT;
    printf("T%02d @ %04d ID %03d RW %01d in room R%02d W%02d in waiting R%02d W%02d pending R %03d W %03d\n",
            threadid,gbVClk,ptr->pID,ptr->RWtype,gbRcnt,gbWcnt,gbRwait,gbWwait,RreqQ.len,WreqQ.len);
    //printf("T%02d @ %04d ID %03d RW %01d in room R%02d W%02d in waiting R%02d W%02d pending R %03d W %03d\n",
    //       threadid,gbVClk,ptr->pID,ptr->RWtype,gbRcnt,gbWcnt,gbRwait,gbLock.waiting_writers,RreqQ.len,WreqQ.len);
    Sleep437(constT2read*1000); //spend X ms to read
    free(ptr);
}
void LeaveReader0(P437 *ptr, int threadid) {
    // Leaving the Room
    gbRcnt=0;
    gbRoomBusy = 0;
    pthread_mutex_unlock(&gbLock);
}

void LeaveReader1(P437 *ptr, int threadid) {
    // Leaving the Room
    rwl_unlock(&myLock);
}
void EnterWriter0(P437 *ptr, int threadid) {
    // try to Enter the room
    pthread_mutex_lock(&gbLock);
    gbRoomBusy = 1;
    gbWcnt=1;
}
void EnterWriter1(P437 *ptr, int threadid) {
    // try to Enter the room
    rwl_wrlock(&myLock);
}

void EnterWriter3(P437 *ptr, int threadid) {
    // try to Enter the room
    rwl_wrlock3(&myLock);
}

void DoWriter(P437 *ptr, int threadid) {
    int wT;
    ptr->deptT = gbVClk;
    wT = ptr->deptT - ptr->arrvT;
    if (wT>data.maxWwait) data.maxWwait=wT; 
    data.numW++; data.sumWwait += wT;
    // Writing
    printf("T%02d @ %04d ID %03d RW %01d in room R%02d W%02d in waiting R%02d W%02d pending R %03d W %03d\n",
            threadid,gbVClk,ptr->pID,ptr->RWtype,gbRcnt,gbWcnt,gbRwait,gbWwait,RreqQ.len,WreqQ.len);
    Sleep437(constT2write*1000); //spend X ms to cross the intersaction
    free(ptr);
}
void LeaveWriter0(P437 *ptr, int threadid) {
    // Leaving the Room
    gbWcnt=0;
    gbRoomBusy = 0;
    pthread_mutex_unlock(&gbLock);
}
void LeaveWriter1(P437 *ptr, int threadid) {
    // Leaving the Room
    rwl_unlock(&myLock);
}

//****************************************************************
// A thread to generate R/W arrival
//      if the pending queue is full, deny the request
//      else Enqueue the arrival, set arrival time, ID, RWtype, etc
//****************************************************************
void *RWcreate(void *vptr) {
    int  k,kk,i,arrivalR,arrivalW,totalArriv,rw,sumR=0,sumW=0;
    P437 *newptr;

    for (kk=k=0;k<timers||QueueEmpty(&RreqQ)==false||QueueEmpty(&WreqQ)==false;k++,kk++) { 
        // synchronize a virtual time to wall clock with 1:1000
        while (GetTime() < kk) Sleep437(1000); // approx granularity 1 msec for 1 sec
        gbVClk += 1; // only place to update our virtual clock 
        // display the waiting line every 10 secs, you can adjust if run for long time
        // taking care of arrival every 10 seconds
        if (k%10==0 && k<timers) { 
            arrivalR = RandPoisson(meanR); sumR+=arrivalR;
            arrivalW = RandPoisson(meanW); sumW+=arrivalW;
            totalArriv = arrivalR+arrivalW;
            for (i=0; i<totalArriv; i++) {
                if (((i%2==0)||arrivalW<=0)&&arrivalR>0) 
                {arrivalR--; rw=false; } // as a Reader 
                else //if (arrivalW>0) 
                {arrivalW--; rw=true; } // as a Writer 
                if (rw&&QueueFull(&WreqQ)) {data.numDeny++;}
                else if (rw==false&&QueueFull(&RreqQ)) {data.numDeny++;}
                else if ((newptr=(P437*)malloc(sizeof(P437)))!=NULL) {
                    newptr->pID = ++gbID; newptr->RWtype=rw; 
                    newptr->arrvT = gbVClk; newptr->deptT = 0;
                    if (rw) 
                    {QueueAppend(&WreqQ,newptr); gbWnum++;}
                    else 
                    {QueueAppend(&RreqQ,newptr); gbRnum++;}
                }
                else {data.numDeny++;}
            }
        }
        if (kk%60==0) {// display for every minute 
            printf("\nCLK %05d RoomBusy %d, waitnum R %02d W %02d, in Room R %02d W %02d pending %d\n",
                    gbVClk,gbRoomBusy,gbRwait,gbWwait,gbRcnt,gbWcnt,RreqQ.len+WreqQ.len);
        }
        // verifying R/W conditions every sec
        assert((gbRcnt==0&&gbWcnt==1) || (gbRcnt>=0&&gbWcnt==0));
    }
}

//****************************************************************
// Multiple threads to process R/W requests from the pending queue
//      if the pending queue is empty, looping to next clk
//      else Dequeue the request to Raed/Write
//****************************************************************
void *Wwork(void *ptr) {
    P437 *pptr; int k, th_id=*(int *)ptr;
    for (k=0;k<timers||QueueEmpty(&WreqQ)==false;k++) { 
        // synchronize a virtual time to wall clock with 1:1000
        while (GetTime() < k) Sleep437(1000); // approx granularity 1 msec
        if (QueueEmpty(&WreqQ)==false&&(pptr=QueuePop(&WreqQ))!=NULL) {
            switch (constPriority) {
                case 0:
                    EnterWriter0(pptr,th_id);
                    DoWriter(pptr,th_id);
                    LeaveWriter0(pptr,th_id);
                    break;
                case 1:
                    EnterWriter1(pptr,th_id);
                    DoWriter(pptr,th_id);
                    LeaveWriter1(pptr,th_id);
                    break;
                case 2:
                    EnterWriter1(pptr,th_id);
                    DoWriter(pptr,th_id);
                    LeaveWriter1(pptr,th_id);
                    break;
                case 3:
                    EnterWriter3(pptr,th_id);
                    DoWriter(pptr,th_id);
                    LeaveWriter1(pptr,th_id);
                    break;
            }
        }
        while (GetTime()>(k+1)) k=GetTime(); // may work overtime, catch up
        pthread_yield();
    }
}
void *Rwork(void *ptr) {
    P437 *pptr; int k, th_id=*(int *)ptr;
    for (k=0;k<timers||QueueEmpty(&RreqQ)==false;k++) { 
        // synchronize a virtual time to wall clock with 1:1000
        while (GetTime() < k) Sleep437(1000); // approx granularity 1 msec
        if (QueueEmpty(&RreqQ)==false&&(pptr=QueuePop(&RreqQ))!=NULL) {
            switch (constPriority) {
                case 0:
                    EnterReader0(pptr,th_id);
                    DoReader(pptr,th_id);
                    LeaveReader0(pptr,th_id);
                    break;
                case 1:
                    EnterReader1(pptr,th_id);
                    DoReader(pptr,th_id);
                    LeaveReader1(pptr,th_id);
                    break;
                case 2:
                    EnterReader2(pptr,th_id);
                    DoReader(pptr,th_id);
                    LeaveReader1(pptr,th_id);
                    break;
                case 3:
                    EnterReader3(pptr,th_id);
                    DoReader(pptr,th_id);
                    LeaveReader1(pptr,th_id);
                    break;
            }
        }
        while (GetTime()>(k+1)) k=GetTime(); // may work overtime, catch up
        pthread_yield();
    }
}

//****************************************************************
// main
//****************************************************************
int main(int argc, char *argv[]) {
    int opt = 0;
    while((opt=getopt(argc,argv,"T:R:W:X:Y:M:C:S:P:")) != -1) switch(opt) {
            case 'T': timers=atoi(optarg);
            break;
            case 'R': meanR = atof(optarg);
            printf("option -R mean arrival: mean=%2.1f \n", meanR);
            break;
            case 'W': meanW = atof(optarg);
            printf("option -W mean arrival: mean=%2.1f \n", meanW);
            break;
            case 'X': constT2read=atoi(optarg);
            printf("option -X Time to read secs =%03ds \n", constT2read);
            break;
            case 'Y': constT2write=atoi(optarg);
            printf("option -Y Time to write secs =%03ds \n", constT2write);
            break;
            case 'M': threadCount=atoi(optarg);
            printf("option -M Number of worker threads =%03ds \n", threadCount);
            break;
            case 'C': data.roomRmax=atoi(optarg);
            printf("option -C Max readers allowed in the room =%03ds \n", data.roomRmax);
            break;
            case 'S': seed=atoi(optarg);
            printf("option -S Random seed =%03ds \n", seed);
            break;
            case 'P': constPriority=atoi(optarg);
            printf("option -P Priority mode =%d \n", constPriority);
            break;
            default:
                fprintf(stderr, "Err: no such option:`%c'\n",optopt);
        }
    if (threadCount <= 0) threadCount = 17;
    int i, numwk=0, workerID[WORKTHREADNUM];
    pthread_t arrv_tid, work_tid[WORKTHREADNUM];
    pthread_attr_t attrs; // try to save memory by getting a smaller stack
    struct rlimit lim; // try to be able to create more threads

    rwl_init(&myLock);
    //rwl_init(&gbWLock);
    //rwl_init(&gbRLock);

    getrlimit(RLIMIT_NPROC, &lim);
    printf("old LIMIT RLIMIT_NPROC soft %d max %d\n",lim.rlim_cur,lim.rlim_max);
    lim.rlim_cur=lim.rlim_max;
    setrlimit(RLIMIT_NPROC, &lim);
    getrlimit(RLIMIT_NPROC, &lim);
    printf("new LIMIT RLIMIT_NPROC soft %d max %d\n",lim.rlim_cur,lim.rlim_max);
    pthread_attr_init(&attrs);
    pthread_attr_setstacksize(&attrs, THREADSTACK); //using 64K stack instead of 2M

    srand(437); InitTime(); // real clock, starting from 0 sec
    //srand(20); InitTime(); // real clock, starting from 0 sec
    data.numR=data.numW=data.numDeny=data.sumRwait=data.sumWwait=0;
    //data.maxRwait=data.maxWwait=data.roomRmax=0;
    data.maxRwait=data.maxWwait=0;

    // Sets args to default if nothing was passed in
    if (meanR <= 0) meanR = 10.0;
    if (timers <= 0) timers = 60*60;
    if (meanW <= 0) meanW = 2.0;
    if (constT2read <= 0) constT2read = 10;
    if (constT2write <= 0) constT2write = 1;
    if (constPriority <= 0) constPriority = 0;
    if (seed <= 0) seed = 437;
    if (data.roomRmax <= 0)  data.roomRmax = 0;


    QueueInit(&WreqQ); QueueInit(&RreqQ); QueueInit(&CombinedQ);
    // simulate 1 hour (60 minutes), between 8:00am-9:00am
    printf("Simulating -R %2.1f/10s -W %2.1f/10s -X %03d -Y %03d -T %ds\n",
            meanR, meanW, constT2read, constT2write, timers);
    // create thread, taking care of arriving
    if (pthread_create(&arrv_tid,&attrs,RWcreate, NULL)) {
        perror("Error in creating arrival thread:");
        exit(1);
    }
    for (i=0; i<3; i++) {
        workerID[i] = i;
        if (pthread_create(&work_tid[i],&attrs,Wwork,&workerID[i])) { 
            perror("Error in creating working threads:");
            work_tid[i]=false;
        }
        else numwk++;
    }
    for (;i<WORKTHREADNUM; i++) {
        workerID[i] = i;
        if (pthread_create(&work_tid[i],&attrs,Rwork,&workerID[i])) { 
            perror("Error in creating working threads:");
            work_tid[i]=false;
        }
        else numwk++;
    }
    printf("Created %d working threads\n",numwk);
    // let simulation run for timers' duration controled by arrival thread
    if (pthread_join(arrv_tid, NULL)) {
        perror("Error in joining arrival thread:");
    }
    for (i=0; i<WORKTHREADNUM; i++) if (work_tid[i]!=false)
        if (pthread_join(work_tid[i],NULL)) {
            perror("Error in joining working thread:");
        }
    if (GetTime()>gbVClk) gbVClk = GetTime();
    // Print Reader/Writer statistics

    printf("\nSim arriv T=%d,done T=%d arrival R %d W %d process R %d W %d deny %d pending %d work thread %d\n",
            timers,gbVClk,gbRnum,gbWnum,data.numR,data.numW,data.numDeny,RreqQ.len+WreqQ.len,numwk);
    // Print waiting statistics
    printf("Waiting time in secs avg R %.1f W %.1f max R %d W %d roomMax R %d\n\n",
            1.0*data.sumRwait/data.numR,
            1.0*data.sumWwait/data.numW,
            data.maxRwait,data.maxWwait,
            data.roomRmax);
}
