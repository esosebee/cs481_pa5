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
#define WORKTHREADNUM 17
#define THREADSTACK  65536

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
Q437 RreqQ, WreqQ;

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

P437* QueueTop(Q437 *ptrQ) {
  // Post: if the Queue is not empty the front of the Queue is returned
    if (ptrQ->len == 0) return NULL;
    else return(ptrQ->entry[ptrQ->front]);
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

// option parameter, set once
    int constT2read=10; //spend 10s to read
    int constT2write=1; //spend 1s to write
    int constPriority=0; //
    int timers=60*60; // default one hour, real time about 4-10 secs
    double meanR=10.0, meanW = 2.0, gbTstart;

    static pthread_mutex_t gbLock = PTHREAD_MUTEX_INITIALIZER; 
    static pthread_mutex_t gbRLock = PTHREAD_MUTEX_INITIALIZER; 
    static pthread_mutex_t gbWLock = PTHREAD_MUTEX_INITIALIZER; 
    static pthread_cond_t gbRoomSem = PTHREAD_COND_INITIALIZER;

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
    // try to Enter the room
    pthread_mutex_lock(&gbLock);
    gbRoomBusy = 1; gbRcnt=1;
    if (gbRcnt>data.roomRmax) data.roomRmax = gbRcnt;
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
    Sleep437(constT2read*1000); //spend X ms to read 
    free(ptr);
}
void LeaveReader0(P437 *ptr, int threadid) {
    // Leaving the Room
    gbRcnt=0;
    gbRoomBusy = 0;
    pthread_mutex_unlock(&gbLock);
}

void EnterWriter0(P437 *ptr, int threadid) {
    // try to Enter the room
    pthread_mutex_lock(&gbLock);
    gbRoomBusy = 1;
    gbWcnt=1;
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
             break;
         case 2:
         case 3:
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
         case 2:
         case 3:
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
    int i, numwk=0, workerID[WORKTHREADNUM], opt;
    pthread_t arrv_tid, work_tid[WORKTHREADNUM];
    pthread_attr_t attrs; // try to save memory by getting a smaller stack
    struct rlimit lim; // try to be able to create more threads

    getrlimit(RLIMIT_NPROC, &lim);
    printf("old LIMIT RLIMIT_NPROC soft %d max %d\n",lim.rlim_cur,lim.rlim_max);
    lim.rlim_cur=lim.rlim_max;
    setrlimit(RLIMIT_NPROC, &lim);
    getrlimit(RLIMIT_NPROC, &lim);
    printf("new LIMIT RLIMIT_NPROC soft %d max %d\n",lim.rlim_cur,lim.rlim_max);
    pthread_attr_init(&attrs);
    pthread_attr_setstacksize(&attrs, THREADSTACK); //using 64K stack instead of 2M

    srand(437); InitTime(); // real clock, starting from 0 sec
    data.numR=data.numW=data.numDeny=data.sumRwait=data.sumWwait=0;
    data.maxRwait=data.maxWwait=data.roomRmax=0;

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
      case 'P': constPriority=atoi(optarg);
        printf("option -P Priority mode =%d \n", constPriority);
        break;
      default:
        fprintf(stderr, "Err: no such option:`%c'\n",optopt);
     }

    QueueInit(&WreqQ); QueueInit(&RreqQ);
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
    if (GetTime() > gbVClk) gbVClk = GetTime();
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
 