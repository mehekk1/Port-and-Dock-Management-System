#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define MAX_AUTH_STRING_LEN 100
#define MAX_CARGO_COUNT 200
#define MAX_NEW_REQUESTS 100
#define MAX_DOCKS 30
#define MAX_SHIPS 1100  
#define MAX_SOLVERS 8

typedef struct Ship {
    int shipId;
    int direction;
    int category;
    int emergency;
    int waitingTime;
    int arrivalTimestep;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
    bool cargoMoved[MAX_CARGO_COUNT];
    int assignedDock;
    bool isDocked;
    bool isServiced;
    bool hasLeft;
} Ship;

typedef struct Dock {
    int category;
    int craneCapacities[25]; // Maximum possible cranes is 25 (dock category maximum)
    int occupiedByShipId;
    int occupiedByDirection;
    bool isEmergency;
    bool allCargoMoved;
    int shipIndex;
    bool canbeundocked;
    int lastcargotime;
    int dockedtime;
} Dock;

typedef struct MessageStruct {
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

typedef struct ShipRequest{
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory{
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

typedef struct SolverRequest{
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse{
    long mtype;
    int guessIsCorrect;
} SolverResponse;


typedef struct {
    int solverId;
    int dockid;
    int strlen;
    int solver_qid;
    char* result;
    atomic_bool* found;
    pthread_mutex_t* result_lock;
} ThreadArgs;

int mainMsgQueueId, shmId;
MainSharedMemory *sharedMemory;
int numSolvers;
int *solverMsgQueueIds;
int numDocks;
Ship incomingShips[500];
Ship outgoingShips[500];
Ship emergencyShips[100];
int incomingIndex = 0;
int outgoingIndex = 0;
int emergencyIndex = 0;
int emergencyLeft = 0;
int timestep=1;
int currentShipIndex=0;
Dock docks[30];
const char valid_chars[] = {'5', '6', '7', '8', '9', '.','\0'};
void ipc_setup(char x){
    printf("DOne till here.\n");
    char inputFilePath[100];
    sprintf(inputFilePath, "testcase%c/input.txt", x);
    
      // Open input file
    FILE *inputFile = fopen(inputFilePath, "r");
    if (inputFile == NULL) {
        perror("Error opening input file");
        exit(EXIT_FAILURE);
    }
    
      // Read shared memory key and main message queue key
    key_t shmKey, mainMsgQueueKey;
    fscanf(inputFile, "%d", &shmKey);
    fscanf(inputFile, "%d", &mainMsgQueueKey);
    
      // Connect to shared memory
    shmId = shmget(shmKey, sizeof(MainSharedMemory), 0666);
    if (shmId == -1) {
        perror("Error connecting to shared memory");
        fclose(inputFile);
        exit(EXIT_FAILURE);
    }
    
      // Attach to shared memory
    sharedMemory = (MainSharedMemory *)shmat(shmId, NULL, 0);
    if (sharedMemory == (void *)-1) {
        perror("Error attaching shared memory");
        fclose(inputFile);
        exit(EXIT_FAILURE);
    }
    
      // Connect to main message queue
    mainMsgQueueId = msgget(mainMsgQueueKey, 0666);
    if (mainMsgQueueId == -1) {
        perror("Error connecting to main message queue");
        fclose(inputFile);
        exit(EXIT_FAILURE);
    }
    
      // Read number of solvers
    fscanf(inputFile, "%d", &numSolvers);
    
      // Allocate memory for solver message queue IDs
    solverMsgQueueIds = (int *)malloc(numSolvers * sizeof(int));
    if (solverMsgQueueIds == NULL) {
        perror("Error allocating memory for solver message queue IDs");
        fclose(inputFile);
        exit(EXIT_FAILURE);
    }
    
      // Connect to solver message queues
    for (int i = 0; i < numSolvers; i++) {
        key_t solverMsgQueueKey;
        fscanf(inputFile, "%d", &solverMsgQueueKey);
        
        solverMsgQueueIds[i] = msgget(solverMsgQueueKey, 0666);
        if (solverMsgQueueIds[i] == -1) {
            perror("Error connecting to solver message queue");
            fclose(inputFile);
            exit(EXIT_FAILURE);
        }
    }
    
    fscanf(inputFile, "%d", &numDocks);

    for (int i = 0; i < numDocks; i++) {
        
        fscanf(inputFile, "%d", &docks[i].category);
        docks[i].occupiedByShipId=-1;
        docks[i].canbeundocked=false;
        
        for (int j = 0; j < docks[i].category; j++) {
            fscanf(inputFile, "%d", &docks[i].craneCapacities[j]);
        }
        
       
    }
    
    fclose(inputFile);

}

void getNewRequest(int n){
    printf("At timestep %d, request recieved %d\n",timestep,n);
    for(int i=0;i<n;i++){
    ShipRequest request = sharedMemory->newShipRequests[i];
    printf("shipid: %d direction: %d emergency:%d\n",request.shipId,request.direction,request.emergency);
    if(request.direction==-1){
        outgoingShips[outgoingIndex].assignedDock=-1;
        outgoingShips[outgoingIndex].shipId=request.shipId;
        outgoingShips[outgoingIndex].category=request.category;
        outgoingShips[outgoingIndex].numCargo=request.numCargo;
        outgoingShips[outgoingIndex].arrivalTimestep=request.timestep;
        outgoingShips[outgoingIndex].isDocked=false;
        outgoingShips[outgoingIndex].direction=-1;
        outgoingShips[outgoingIndex].emergency=0;
        for(int j=0;j<request.numCargo;j++){
            outgoingShips[outgoingIndex].cargo[j]=request.cargo[j];
            outgoingShips[outgoingIndex].cargoMoved[j] = false;
        }
        outgoingIndex++;

    }else if(request.direction==1 && request.emergency==1){
        emergencyShips[emergencyIndex].assignedDock=-1;
        emergencyShips[emergencyIndex].category=request.category;
        emergencyShips[emergencyIndex].shipId=request.shipId;
        emergencyShips[emergencyIndex].numCargo=request.numCargo;
        emergencyShips[emergencyIndex].arrivalTimestep=request.timestep;
        emergencyShips[emergencyIndex].isDocked=false;
        emergencyShips[emergencyIndex].direction=1;
        emergencyShips[emergencyIndex].emergency=1;
        for(int j=0;j<request.numCargo;j++){
            emergencyShips[emergencyIndex].cargo[j]=request.cargo[j];
            emergencyShips[emergencyIndex].cargoMoved[j] = false;
        }
        emergencyIndex++;
        emergencyLeft++;
    }else if(request.direction==1 && request.emergency==0){
        for(int j=0;j<=incomingIndex;j++){
            if(j==incomingIndex){
                incomingShips[j].shipId=request.shipId;
                incomingShips[j].arrivalTimestep=request.timestep;
                incomingShips[j].numCargo=request.numCargo;
                incomingShips[j].waitingTime=request.waitingTime;
                incomingShips[j].isDocked=false;
                incomingShips[j].direction=1;
                incomingShips[j].emergency=0;
                incomingShips[j].category=request.category;
                for(int k=0;k<request.numCargo;k++){
                    incomingShips[j].cargo[k]=request.cargo[k];
                    incomingShips[j].cargoMoved[k] = false;
                }
                incomingIndex++;
                break;
            }else{
                if(request.shipId==incomingShips[j].shipId){
                    incomingShips[j].arrivalTimestep=request.timestep;
                    incomingShips[j].waitingTime=request.waitingTime;
                    break;

                }
            }
        }

    }
    }

}
void dockship(int dockid){
    bool isdocked=false;
    if(emergencyLeft>0){
        int maxcat=-1;
        int maxind=-1;
        for(int i=0;i<emergencyIndex;i++){
            if(emergencyShips[i].isDocked==false && emergencyShips[i].category<=docks[dockid].category){
                if(emergencyShips[i].category>maxcat){
                    maxcat=emergencyShips[i].category;
                    maxind=i;
                }
            }

        }
        if(maxcat!=-1 && maxind!=-1){
            MessageStruct msg;
            msg.mtype=2;
            msg.dockId=dockid;
            msg.shipId=emergencyShips[maxind].shipId;
            msg.direction=emergencyShips[maxind].direction;
            if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending docking message.");
                return;
            }
            isdocked=true;
            emergencyLeft--;
            emergencyShips[maxind].isDocked=true;
            docks[dockid].isEmergency=true;
            docks[dockid].shipIndex=maxind;
            docks[dockid].occupiedByShipId=emergencyShips[maxind].shipId;
            docks[dockid].occupiedByDirection=emergencyShips[maxind].direction;
            docks[dockid].dockedtime=timestep;
            
        }
    }
    if(isdocked==false){
        int maxcat=-1;
        int maxind=-1;
        for(int i=0;i<incomingIndex;i++){
            if(incomingShips[i].isDocked==false && incomingShips[i].category<=docks[dockid].category){
                if(incomingShips[i].category>maxcat && ((incomingShips[i].arrivalTimestep+incomingShips[i].waitingTime)>=timestep)){
                    maxcat=incomingShips[i].category;
                    maxind=i;
                }
            }

        }
        if(maxcat!=-1 && maxind!=-1){
            MessageStruct msg;
            msg.mtype=2;
            msg.dockId=dockid;
            msg.shipId=incomingShips[maxind].shipId;
            msg.direction=incomingShips[maxind].direction;
            if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending docking message.");
                return;
            }
            isdocked=true;
            incomingShips[maxind].isDocked=true;
            docks[dockid].isEmergency=false;
            docks[dockid].shipIndex=maxind;
            docks[dockid].occupiedByShipId=incomingShips[maxind].shipId;
            docks[dockid].occupiedByDirection=incomingShips[maxind].direction;
            docks[dockid].dockedtime=timestep;
            
        }
    }
    if(isdocked==false){
        int maxcat=-1;
        int maxind=-1;
        for(int i=0;i<outgoingIndex;i++){
            if(outgoingShips[i].isDocked==false){
                if(outgoingShips[i].category>maxcat && outgoingShips[i].category<=docks[dockid].category){
                    maxcat=outgoingShips[i].category;
                    maxind=i;
                }
            }
        }
        if(maxcat!=-1 && maxind!=-1){
            MessageStruct msg;
            msg.mtype=2;
            msg.dockId=dockid;
            msg.shipId=outgoingShips[maxind].shipId;
            msg.direction=outgoingShips[maxind].direction;
            if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending docking message.");
                return;
            }
            isdocked=true;
            outgoingShips[maxind].isDocked=true;
            docks[dockid].isEmergency=false;
            docks[dockid].shipIndex=maxind;
            docks[dockid].occupiedByShipId=outgoingShips[maxind].shipId;
            docks[dockid].occupiedByDirection=outgoingShips[maxind].direction;
            docks[dockid].dockedtime=timestep;   
        }
    }
}
void unload(int dockid){
    for(int i=0;i<docks[dockid].category;i++){
        int cargoid=-1;
        int cargocat=-1;
        if(docks[dockid].isEmergency){
        for(int j=0;j<emergencyShips[docks[dockid].shipIndex].numCargo;j++){
            if(emergencyShips[docks[dockid].shipIndex].cargo[j]<=docks[dockid].craneCapacities[i] && 
                emergencyShips[docks[dockid].shipIndex].cargo[j]>cargocat &&
            emergencyShips[docks[dockid].shipIndex].cargoMoved[j]==false){
                    cargocat=emergencyShips[docks[dockid].shipIndex].cargo[j];
                    cargoid=j;


            }

        }
        if(cargoid!=-1 && cargocat!=-1){
            MessageStruct msg;
            msg.mtype = 4;
            msg.shipId = docks[dockid].occupiedByShipId;
            msg.direction = 1;
            msg.dockId = dockid;
            msg.cargoId = cargoid;
            msg.craneId = i;

            if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending cargo move message");
                return;
            }
            emergencyShips[docks[dockid].shipIndex].cargoMoved[cargoid]=true;

        }
    }else if(docks[dockid].occupiedByDirection==-1){
        for(int j=0;j<outgoingShips[docks[dockid].shipIndex].numCargo;j++){
            if(outgoingShips[docks[dockid].shipIndex].cargo[j]<=docks[dockid].craneCapacities[i] && 
                outgoingShips[docks[dockid].shipIndex].cargo[j]>cargocat &&
            outgoingShips[docks[dockid].shipIndex].cargoMoved[j]==false){
                    cargocat=outgoingShips[docks[dockid].shipIndex].cargo[j];
                    cargoid=j;


            }

        }
        if(cargoid!=-1 && cargocat!=-1){
            MessageStruct msg;
            msg.mtype = 4;
            msg.shipId = docks[dockid].occupiedByShipId;
            msg.direction = -1;
            msg.dockId = dockid;
            msg.cargoId = cargoid;
            msg.craneId = i;

            if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending cargo move message");
                return;
            }
            outgoingShips[docks[dockid].shipIndex].cargoMoved[cargoid]=true;

        }
    }else{
        for(int j=0;j<incomingShips[docks[dockid].shipIndex].numCargo;j++){
            if(incomingShips[docks[dockid].shipIndex].cargo[j]<=docks[dockid].craneCapacities[i] && 
                incomingShips[docks[dockid].shipIndex].cargo[j]>cargocat &&
                incomingShips[docks[dockid].shipIndex].cargoMoved[j]==false){
                    cargocat=incomingShips[docks[dockid].shipIndex].cargo[j];
                    cargoid=j;


            }

        }
        if(cargoid!=-1 && cargocat!=-1){
            MessageStruct msg;
            msg.mtype = 4;
            msg.shipId = docks[dockid].occupiedByShipId;
            msg.direction = 1;
            msg.dockId = dockid;
            msg.cargoId = cargoid;
            msg.craneId = i;

            if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                perror("Error sending cargo move message");
                return;
            }
            incomingShips[docks[dockid].shipIndex].cargoMoved[cargoid]=true;

        }

    }
    }
    docks[dockid].canbeundocked=true;
    if(docks[dockid].isEmergency==true){
    for(int i=0;i<emergencyShips[docks[dockid].shipIndex].numCargo;i++){
        if(emergencyShips[docks[dockid].shipIndex].cargoMoved[i]==false){
            docks[dockid].canbeundocked=false;
            break;
        }
    }
    }else if(docks[dockid].occupiedByDirection==1){
        for(int i=0;i<incomingShips[docks[dockid].shipIndex].numCargo;i++){
            if(incomingShips[docks[dockid].shipIndex].cargoMoved[i]==false){
                docks[dockid].canbeundocked=false;
                break;
            }
        }
    }else if(docks[dockid].occupiedByDirection==-1){
        for(int i=0;i<outgoingShips[docks[dockid].shipIndex].numCargo;i++){
            if(outgoingShips[docks[dockid].shipIndex].cargoMoved[i]==false){
                docks[dockid].canbeundocked=false;
                break;
            }
        }
        }
        if(docks[dockid].canbeundocked==true){
            docks[dockid].lastcargotime=timestep;
        }

}

int power(int a,int b){
    int t = 1;
    while(b--){
        t *= a;
    }
    return t;
}
int isValidGuess(const char *str, int len) {
    return str[0] != '.' && str[len - 1] != '.';
}

void indexTostr(int index,int len,char* ret){
    for(int i= len-1; i >= 0; i--){
        ret[i] = valid_chars[index%6];
        index /= 6;
    }
    ret[len] = '\0';
}


void* guesserThread(void* arg) {
    ThreadArgs* t = (ThreadArgs*) arg;
    char guess[MAX_AUTH_STRING_LEN];
    memset(guess, '5', t->strlen);
    guess[t->strlen] = '\0';

    int total = power(6, t->strlen);

    for (int i = 0; i < total && !*(t->found); i++) {
        if (i % numSolvers != t->solverId) continue;

        indexTostr(i, t->strlen, guess);
        if (!isValidGuess(guess, t->strlen)) continue;

        SolverRequest req = {.mtype = 2, .dockId = t->dockid};
        strncpy(req.authStringGuess, guess, MAX_AUTH_STRING_LEN);

        if (msgsnd(t->solver_qid, &req, sizeof(SolverRequest) - sizeof(long), 0) == -1) exit(1);

        SolverResponse resp;
        if (msgrcv(t->solver_qid, &resp, sizeof(SolverResponse) - sizeof(long), 3, 0) == -1) exit(1);

        if (resp.guessIsCorrect == 1) {
            pthread_mutex_lock(t->result_lock);
            if (!*(t->found)) {
                *(t->found) = true;
                strncpy(t->result, guess, MAX_AUTH_STRING_LEN);
            }
            pthread_mutex_unlock(t->result_lock);
            break;
        }
    }

    return NULL;
}

void generateNextGuess(char *str, int len, int pos) {
    static const char chars[] = {'5', '6', '7', '8', '9', '.','\0'};
    int i = pos;
    while (i >= 0) {
        const char *p = strchr(chars, str[i]);
        if (p == NULL || p == chars + 5) {
            str[i] = chars[0];
            i--;
        } else {
            str[i] = *(p + 1);
            break;
        }
    }
}

void undock(int dockid) {
    int strlen = docks[dockid].lastcargotime - docks[dockid].dockedtime;
    if (strlen >= MAX_AUTH_STRING_LEN - 1) strlen = MAX_AUTH_STRING_LEN - 1;

    pthread_t threads[MAX_SOLVERS];
    ThreadArgs args[MAX_SOLVERS];
    char result[MAX_AUTH_STRING_LEN];
    atomic_bool found = false;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    SolverRequest req = {.mtype = 1, .dockId = dockid};
    for (int i = 0; i < numSolvers; i++) {
        msgsnd(solverMsgQueueIds[i], &req, sizeof(SolverRequest) - sizeof(long), 0);
    }

    // Spawn solver threads
    for (int i = 0; i < numSolvers; i++) {
        args[i] = (ThreadArgs){
            .solverId = i,
            .dockid = dockid,
            .strlen = strlen,
            .solver_qid = solverMsgQueueIds[i],
            .result = result,
            .found = &found,
            .result_lock = &lock
        };
        pthread_create(&threads[i], NULL, guesserThread, &args[i]);
    }

    for (int i = 0; i < numSolvers; i++) pthread_join(threads[i], NULL);

    // If found, send undock msg
    if (found) {
        strncpy(sharedMemory->authStrings[dockid], result, MAX_AUTH_STRING_LEN);

        MessageStruct msg = {
            .mtype = 3,
            .dockId = dockid,
            .shipId = docks[dockid].occupiedByShipId,
            .direction = docks[dockid].occupiedByDirection
        };

        if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) != -1) {
            docks[dockid].occupiedByShipId = -1;
            docks[dockid].canbeundocked = false;
        }
    }
}

void shipprocessing(){
    for(int i=0;i<numDocks;i++){
        if(docks[i].canbeundocked==true){
            printf("undock dock: %d\n",i);
            undock(i);
        }else if(docks[i].occupiedByShipId==-1){
            dockship(i);
        }else{
            unload(i);
        }
    }

}

int main(int argc, char *argv[]) {
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        exit(1);
    }

    
    ipc_setup(*argv[1]);
    

    while(true){
        MessageStruct msg;
        
        if (msgrcv(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 1, 0) == -1) {
            perror("Error receiving message from validation");
        
            return EXIT_FAILURE;
        }
        
          // Check if test case is finished
        if (msg.isFinished == 1) {
            break;
        }

        getNewRequest(msg.numShipRequests);
        shipprocessing();
        msg.mtype=5;
        if (msgsnd(mainMsgQueueId, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
            perror("main function error.");
            return EXIT_FAILURE;
        }

        timestep++;



    }

}
