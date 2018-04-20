/*
 * Name:    proj2.c
 * Author:  Jiri Peska
 * Login:   xpeska05
 * Date:    27.4.2017
 * Version: gcc version 6.3.0 20170205 (Debian 6.3.0-6)
 * Compile: gcc -std=gnu99 -Wall -Wextra -Werror -pedantic proj2.c -o proj2 -lrt -lpthread
 * Usage:   $ ./proj2 A C AGT CGT AWT CWT
 * ZIP:     zip proj2 *
 * Description: Program vygeneruje A dospelych a C deti, kteri chodi v nahodne dobe do centra,
 * do nehoz je pristup rizen podminkou, ze na kazdeho dospeleho mohou byt maximalne 3 deti.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>

/* Pomocna makra */
#define SAFE_PRINT(STREAM,...)   do {sem_wait(semData); fprintf(STREAM, __VA_ARGS__); fflush(STREAM); sem_post(semData);} while (0);
#define FATAL_ERROR(STREAM, ...) do {fprintf(STREAM, __VA_ARGS__); exit(1); } while(0);

/* struktura pro argumenty programu */
typedef struct args
{
     unsigned int A;
     unsigned int C;
     unsigned int AGT;
     unsigned int CGT;
     unsigned int CWT;
     unsigned int AWT;
} args_t;

/* struktura reprezentujici sdilenou pamet */
typedef struct SharedData
{
     unsigned int counter;   // pocitadlo pro vypis

     unsigned int C_inside;  // pocet deti uvnitr centra
     unsigned int C_waiting; // pocet deti cekajici na vpusteni

     unsigned int A_inside;  // pocet dospelych uvnitr centra
     unsigned int A_leaving; // pocet dospelych cekajici na odchod

     unsigned int A_left;    // pocet dospelych, kteri uz odesli - kvuli kontrole
} sharedData_t;

/* semafory */
sem_t *semChildQ, *semAdultQ, *semData, *semMutex, *semFinish, *semMainFinish;

/* pids */
pid_t pidc, pida, childPid, adultPid, adultGenPid, childGenPid;

/* sdilena pamet */
sharedData_t *centreInfo = NULL;
int centreInfoId;

/* dalsi potrebne veci */
FILE *fout = NULL;
args_t args;

/* deklarace funkci */
void childProcess(int n);
void adultProcess(int n);
void closeSharedMemory();
void closeSemaphores();
void getArgs(int argc, char *argv[], args_t *argumenty);


/* ********************************** MAIN ********************************************** */
int main(int argc, char *argv[])
{
     srand(time(0));
     getArgs(argc, argv, &args);

     if((fout = fopen("proj2.out", "w")) == NULL)
     {
          FATAL_ERROR(stderr, "Chyba: Nelze otevrit soubor pro zapis, %d\n", errno);
     }

     //vypneme buffer
     setbuf(fout, NULL);

     //vytvorime sdilenou pamet
     centreInfoId = shm_open("/xpeska05_shared", O_CREAT | O_EXCL | O_RDWR, 0644);
     if(centreInfoId < 0) FATAL_ERROR(stderr, "Nelze vytvorit sdilenou pamet, %d\n", errno);
     ftruncate(centreInfoId, sizeof(sharedData_t));
     centreInfo = mmap(NULL, sizeof(sharedData_t), PROT_READ | PROT_WRITE, MAP_SHARED, centreInfoId, 0);

     //inicializace sdilenych promennych
     centreInfo->counter      = 1;
     centreInfo->C_inside     = 0;
     centreInfo->A_inside     = 0;
     centreInfo->C_waiting    = 0;
     centreInfo->A_leaving    = 0;
     centreInfo->A_left       = 0;

     //vytvoreni semaforu, kde Locked = 0     Unlocked > 0
     semChildQ      = sem_open("/xpeska05_child_q",  O_CREAT | O_EXCL, 0666, 0);
     semAdultQ      = sem_open("/xpeska05_adult_q",  O_CREAT | O_EXCL, 0666, 0);
     semData        = sem_open("/xpeska05_data",   O_CREAT | O_EXCL, 0666, 1);
     semMutex       = sem_open("/xpeska05_mutex",  O_CREAT | O_EXCL, 0666, 1);
     semFinish      = sem_open("/xpeska05_finish", O_CREAT | O_EXCL, 0666, 0);
     semMainFinish  = sem_open("/xpeska05_main_finish", O_CREAT | O_EXCL, 0666, 0);

     if(semChildQ == SEM_FAILED
     || semAdultQ == SEM_FAILED
     || semData == SEM_FAILED
     || semMutex == SEM_FAILED
     || semFinish == SEM_FAILED
     || semMainFinish == SEM_FAILED)
     {
          //zavrit otevrene deskriptory, pak ukoncit
          closeSharedMemory();
          closeSemaphores();
          FATAL_ERROR(stderr, "Chyba pri vytvareni semaforu.\n");
     }

     /* ===================================================================================== */
     /* =========== proces, co bude generovat Adults ========== */
     adultGenPid = fork();
     if(adultGenPid == -1)
     {
          closeSharedMemory();
          closeSemaphores();
          FATAL_ERROR(stderr, "Chyba behem vytvareni pomocneho procesu pro dospele.\n");
     }
     else if(adultGenPid == 0)
     {
          /* ======== Generovani DOSPELYCH ======== */
          for(unsigned int j = 1; j <= args.A; j++)
          {
               if(args.AGT > 0)
               usleep(rand() % args.AGT * 1000);

               pida = fork();
               if(pida == -1)
               {
                    closeSharedMemory();
                    closeSemaphores();
                    FATAL_ERROR(stderr, "Chyba behem vytvareni procesu dospeleho.\n");
               }
               else if(pida == 0)
               {
                    // V konkretnim dospelem:
                    adultProcess(j);
               }
          }

          //pomocny proces ceka, az se ukonci vsechny deti
          for(unsigned int i = 0; i < args.A; i++) wait(NULL);

          //SAFE_PRINT(fout, "Konci pomocny proces pro generovani ADULT\n");
          exit(0);
     }

     /* ===================================================================================== */
     //CHILDREN
     childGenPid = fork();
     if(childGenPid == -1)
     {
          closeSharedMemory();
          closeSemaphores();
          FATAL_ERROR(stderr, "Chyba behem vytvareni pomocneho procesu pro deti.\n");
     }
     else if(childGenPid == 0)
     {
          /* ======== Generovani DETI ======== */
          for(unsigned int i = 1; i <= args.C; i++)
          {
               if(args.CGT > 0)
               usleep(rand() % args.CGT * 1000);

               pidc = fork();
               if(pidc == -1)
               {
                    closeSharedMemory();
                    closeSemaphores();
                    FATAL_ERROR(stderr, "Chyba behem vytvareni procesu ditete.\n");
               }
               //v diteti:
               else if(pidc == 0)
               {
                    childProcess(i);
               }
          }

          //pomocny proces ceka, az se ukonci vsechny deti
          for(unsigned int i = 0; i < args.C; i++) wait(NULL);

          //SAFE_PRINT(fout, "Konci pomocny proces pro generovani CHILD\n");
          exit(0);
     }

     //toto zablokuje main, procesy ho budou postupne uvolnovat
     for(unsigned int i = 0; i < (args.A+args.C); i++)
          sem_wait(semMainFinish);

     //po uvolneni se povoli finishnout ostatnim procesum
     for(unsigned int i = 0; i < args.A + args.C; i++)
          sem_post(semFinish);

     //vsechyn procesy skoncily, uz muzou skoncit i pomocne generatory
     waitpid(adultGenPid, NULL, 0);
     waitpid(childGenPid, NULL, 0);

     //SAFE_PRINT(fout, "Konci main\n");

     //zavirame semafory
     closeSharedMemory();
     closeSemaphores();

     return 0;
}

/* **********************************************************************************************/
/*
 * Proces pro dite
 * n   cislo ditete
 */
void childProcess(int n)
{
     SAFE_PRINT(fout, "%-2d     : C %d   : started\n", (centreInfo->counter)++, n);

     sem_wait(semMutex);
     //osetreni pri vycerpanych dospelych
     if(centreInfo->A_left != args.A)
     {
          if(centreInfo->C_inside < ((centreInfo->A_inside+centreInfo->A_leaving) * 3)) //?-------
          {
               centreInfo->C_inside++;
               SAFE_PRINT(fout, "%-2d     : C %d   : enter\n", (centreInfo->counter)++, n);
               sem_post(semMutex);
          }
          else
          {
                    centreInfo->C_waiting++;
                    SAFE_PRINT(fout, "%-2d     : C %d   : waiting: %d: %d\n", (centreInfo->counter)++, n, centreInfo->A_inside+centreInfo->A_leaving, centreInfo->C_inside);
                    sem_post(semMutex);
                    sem_wait(semChildQ); //child queue

                    //po vpusteni Adult procesem
                    SAFE_PRINT(fout, "%-2d     : C %d   : enter\n", (centreInfo->counter)++, n);
          }
     }
     else
     {
          SAFE_PRINT(fout, "%-2d     : C %d   : enter\n", (centreInfo->counter)++, n);
          sem_post(semMutex);
     }


     // <critical_section>
     if(args.CWT > 0)
          usleep(rand() % args.CWT * 1000);
     // </critical_section>


     sem_wait(semMutex);
     centreInfo->C_inside--;
     SAFE_PRINT(fout, "%-2d     : C %d   : trying to leave\n", (centreInfo->counter)++, n);
     SAFE_PRINT(fout, "%-2d     : C %d   : leave\n", (centreInfo->counter)++, n);

     if(centreInfo->A_leaving>0 && centreInfo->C_inside <= 3* (centreInfo->A_inside + centreInfo->A_leaving-1)) // -1
     {
          centreInfo->A_left++;
          centreInfo->A_leaving--;
          sem_post(semAdultQ); // adult queue
     }
     sem_post(semMutex);

     sem_post(semMainFinish);
     sem_wait(semFinish);
     SAFE_PRINT(fout, "%-2d     : C %d   : finished\n", (centreInfo->counter)++, n);

     exit(0);
}

/* **********************************************************************************************/

/*
 * Proces pro dospeleho
 * n   cislo adulta
 */
void adultProcess(int n)
{
     SAFE_PRINT(fout, "%-2d     : A %d   : started\n", (centreInfo->counter)++, n);

     sem_wait(semMutex);
     SAFE_PRINT(fout, "%-2d     : A %d   : enter\n", (centreInfo->counter)++, n); /// nebo prohodit s radkem vys???
     centreInfo->A_inside++;
     if(centreInfo->C_waiting > 0)
     {
          int max = centreInfo->C_waiting;
          if(max > 3) { max = 3; }
          centreInfo->C_waiting -= max;
          centreInfo->C_inside  += max;
          for(int p = 0; p < max; p++)
          {
               sem_post(semChildQ);
          }
     }
     sem_post(semMutex);


     // <critical_section>
     if(args.AWT > 0)
          usleep(rand() % args.AWT * 1000);
     // </critical_section>


     sem_wait(semMutex);
     SAFE_PRINT(fout, "%-2d     : A %d   : trying to leave\n", (centreInfo->counter)++, n);
     if(centreInfo->C_inside <= 3 * (centreInfo->A_inside+centreInfo->A_leaving -1))
     {
          SAFE_PRINT(fout, "%-2d     : A %d   : leave\n", (centreInfo->counter)++, n);
          centreInfo->A_left++;
          centreInfo->A_inside--;
          sem_post(semMutex);
     }
     else
     {
          centreInfo->A_leaving++;
          centreInfo->A_inside--;
          SAFE_PRINT(fout, "%-2d     : A %d   : waiting: %d: %d\n", (centreInfo->counter)++, n, centreInfo->A_inside+centreInfo->A_leaving, centreInfo->C_inside);
          sem_post(semMutex);
          sem_wait(semAdultQ);
          SAFE_PRINT(fout, "%-2d     : A %d   : leave\n", (centreInfo->counter)++, n);
     }

     //waitpid(getppid(), NULL, 0);
     sem_post(semMainFinish);
     sem_wait(semFinish);
     SAFE_PRINT(fout, "%-2d     : A %d   : finished\n", (centreInfo->counter)++, n);

     exit(0);
}
/* **********************************************************************************************/

/*
 * Zavre sdilenou pamet
 */
void closeSharedMemory()
{
     close(centreInfoId);
     munmap(centreInfo, sizeof(sharedData_t));
     shm_unlink("/xpeska05_shared");
     shmctl(centreInfoId, IPC_RMID, NULL);
}

/*
 * Zavre semafory
 */
void closeSemaphores()
{
     sem_unlink("/xpeska05_child_q");
     sem_unlink("/xpeska05_adult_q");
     sem_unlink("/xpeska05_data");
     sem_unlink("/xpeska05_mutex");
     sem_unlink("/xpeska05_finish");
     sem_unlink("/xpeska05_main_finish");

     sem_close(semChildQ);
     sem_close(semAdultQ);
     sem_close(semData);
     sem_close(semMutex);
     sem_close(semFinish);
     sem_close(semMainFinish);

     fclose(fout);
}

/*
 *zpracujeme argumenty programu
 */
void getArgs(int argc, char *argv[], args_t *argumenty)
{
     if(argc == 7)
     {
          argumenty->C = atoi(argv[2]);
          argumenty->A = atoi(argv[1]);

          int tAGT = atoi(argv[3]);
          int tCGT = atoi(argv[4]);
          int tAWT = atoi(argv[5]);
          int tCWT = atoi(argv[6]);
          if(tAGT >= 0 && tAGT < 5001) argumenty->AGT = tAGT; else FATAL_ERROR(stderr, "Chyba: Hodnota mimo dovoleny rozsah.\n");
          if(tCGT >= 0 && tCGT < 5001) argumenty->CGT = tCGT; else FATAL_ERROR(stderr, "Chyba: Hodnota mimo dovoleny rozsah.\n");
          if(tAWT >= 0 && tAWT < 5001) argumenty->AWT = tAWT; else FATAL_ERROR(stderr, "Chyba: Hodnota mimo dovoleny rozsah.\n");
          if(tCWT >= 0 && tCWT < 5001) argumenty->CWT = tCWT; else FATAL_ERROR(stderr, "Chyba: Hodnota mimo dovoleny rozsah.\n");
     }
     else
     {
          FATAL_ERROR(stderr, "Chyba - nespravny pocet argumentu nebo chyba v argumentu.\n");
     }
}
