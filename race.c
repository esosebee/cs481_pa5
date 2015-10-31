#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct {int balance[2];} Bank={{100,100}}; //global variable defined

/*************/
/* Semaphore */
/*************/
typedef struct
{
  int count;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
} Sem437;

void Sem437Init(Sem437 *S, int x)
{
  S->count = x;
  pthread_mutex_init(&S->mutex, NULL);
  pthread_cond_init(&S->condition, NULL);
}

void Sem437P(Sem437 *S)
{
  pthread_mutex_trylock(&S->mutex);
  while (S->count == 0)
  {
    pthread_cond_wait(&S->condition, &S->mutex);
  }
  S->count = 0;
  pthread_mutex_unlock(&S->mutex);
}

void Sem437V(Sem437 *S)
{
  int id = S->count;
  pthread_mutex_lock(&S->mutex);
  S->count = 1;
  pthread_mutex_unlock(&S->mutex);
  if (id == 0)
  {
    pthread_cond_signal(&S->condition);
  }
}

/****************/
/* Transactions */
/****************/
Sem437 sem;

void* MakeTransactions() //routine for thread execution
{ 
 int i, j, tmp1, tmp2, rint; 
 double dummy;
 Sem437P(&sem);
 for (i = 0; i < 100; i++) // Critical section
  { 
    rint = (rand()%30)-15;
    if (((tmp1=Bank.balance[0]) + rint) >= 0 && ((tmp2=Bank.balance[1])-rint) >= 0) 
    {
      Bank.balance[0] = tmp1 + rint;
      for (j=0; j < rint*100; j++) 
      {
        dummy = 2.345 * 8.765/1.234;
      }
      Bank.balance[1] = tmp2 - rint;
    }
  }
  Sem437V(&sem);
 return NULL;
}

int main(int argc, char **argv) 
{
  int i; void* voidptr=NULL; pthread_t tid[2];
  Sem437Init(&sem, 1);
  srand(getpid());
  
  printf("Init balances A:%d + B:%d ==> %d!\n",
    Bank.balance[0],Bank.balance[1],Bank.balance[0]+Bank.balance[1]);
  
  for (i=0; i<2; i++) if (pthread_create(&tid[i],NULL,MakeTransactions, NULL)) 
  {
    perror("Error in thread creating\n"); 
    return(1); 
  }
  for (i=0; i<2; i++) if (pthread_join(tid[i], (void*)&voidptr)) 
  {
    perror("Error in thread joining\n"); 
    return(1);
  }
  printf("Let's check the balances A:%d + B:%d ==> %d ?= 200\n",
    Bank.balance[0],Bank.balance[1],Bank.balance[0]+Bank.balance[1]);
  return 0;
} 