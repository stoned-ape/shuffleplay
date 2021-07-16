#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>


int ret; //used for storing and checking return values

//wrap all syscalls with this to get a helpfull error message
//if something goes wrong
//this is somewhat counter-intuitive because I do:
//  int pid=SYSCALL(fork());
//which expands to:
//  int pid=ret=fork(); ...
//this is valid syntax because the "=" operator returns the value assigned
#define SYSCALL(call) \
ret=call; \
if(ret==-1){ \
    printf("syscall error: (%s) in function %s at line %d of file %s\n", \
        strerror(errno),__func__,__LINE__,__FILE__); \
    exit(errno); \
}

//pthread library calls must wrapped in a slighly different way for
//good error handling/reporting
#define PTHREAD(call) \
ret=call; \
if(ret!=0){ \
    printf("pthread error: (%s) in function %s at line %d of file %s\n", \
        strerror(ret),__func__,__LINE__,__FILE__); \
    exit(ret); \
}

//#define DEBUG

//this shuffles an array by repeatedly picking two random elements
//and swapping them
void shuffle(char **a,int n){
    srand(time(NULL));
    for(int i=0;i<4*n;i++){
        int j=rand()%n,k=rand()%n;
        char *t=a[j];
        a[j]=a[k];
        a[k]=t;
    }
}

//This function returns an array containing the names of all the files
//in the current directory.  Then length array is return through the
//len parameter.
//This is accomplished by executing ls and redirecting the output into
//a pipe.
char **getlist(int *len){
    int p[2];
    SYSCALL(pipe(p));
    int pid=SYSCALL(fork());
    if(pid==0){
        SYSCALL(dup2(p[1],STDOUT_FILENO));    //redirect stdout into the pipe
        SYSCALL(execl("/bin/ls","ls",NULL));  //execute ls
    }
    int bufsz=1024;
    char *buf=malloc(bufsz);
    memset(buf,0,bufsz);
    int n=SYSCALL(read(p[0],buf,bufsz)); //read output from ls
#ifdef DEBUG
    printf("%s",buf);
#endif
    int status;
    SYSCALL(waitpid(pid,&status,0));
    assert(WEXITSTATUS(status)==0);
    
    int num_songs=0;
    for(int i=0;i<n;i++) if(buf[i]=='\n'){ //ls output is '\n' terminated but ...
        buf[i]=0;  //c-strings must be null-terminated
        num_songs++;
    }
    
    char **songs=malloc(sizeof(char*)*num_songs);
    songs[0]=buf;
    if(num_songs>1){
        int j=1;
        for(int i=0;i<n;i++){
            if(buf[i]==0){  //0 terminates the string
                songs[j]=&buf[i+1]; //thus the next byte begins the next string
                j++;
                if(j>=num_songs) break;
             }
        }
    }
#ifdef DEBUG
    printf("\n");
    for(int i=0;i<num_songs;i++) printf("%s\n",songs[i]);
#endif
    *len=num_songs; //return array length
    return songs;   //return array
}

//stores the state of the program
typedef struct{
    int current_song_pid; //points to an afplay process
    int paused;           //0 if playing, 1 if paused
    int song_idx;         //index of current song in array
    //these synchonization primitives aren't strictly necessary
    //they are use to make the text output act as expected
    pthread_mutex_t mtx1,mtx2;
    pthread_cond_t cv1,cv2;
}state;

//I cant not use object programming
void state_init(state *this){
    assert(this!=NULL);
    this->paused=0;
    this->song_idx=0;
    //initialize synchronization primitives
    PTHREAD(pthread_mutex_init(&this->mtx1,0));
    PTHREAD(pthread_cond_init(&this->cv1,0));
    PTHREAD(pthread_mutex_init(&this->mtx2,0));
    PTHREAD(pthread_cond_init(&this->cv2,0));
}
//just kill the current afplay process to end the song
void state_end_song(state *this){
    assert(this!=NULL);
    SYSCALL(kill(this->current_song_pid,SIGINT));
    PTHREAD(pthread_cond_wait(&this->cv1,&this->mtx1)); //wait for the next title to be printed
    this->paused=0;
}
//play with SIGCONT
void state_play(state *this){
    assert(this!=NULL);
    SYSCALL(kill(this->current_song_pid,SIGCONT));
    this->paused=0;
}
//pause with SIGSTOP
void state_pause(state *this){
    assert(this!=NULL);
    SYSCALL(kill(this->current_song_pid,SIGSTOP));
    this->paused=1;
}

pthread_mutex_t print_mtx;

//wrap code with this macro to make it atomic with mutexes
#define ATOMIC(code){ \
    PTHREAD(pthread_mutex_lock(&print_mtx)); \
    code; \
    PTHREAD(pthread_mutex_unlock(&print_mtx)); \
}
    
//I'm using the macOS binary, afplay, to play music.
int play_song(char *name,state *st){
    st->current_song_pid=SYSCALL(fork());
    if(st->current_song_pid==0){
        SYSCALL(execl("/usr/bin/afplay","afplay",name,NULL));
    }
    int status;
    SYSCALL(waitpid(st->current_song_pid,&status,0));
    return status;
}

#define max(a,b) ((a)>(b)?(a):(b))

//This function is run by a separate thread to get user input.
//it allows you to skip (n) and pause/unpause (p) songs.
//skipping is as simple as killing afplay
//pause/unpause is implemented with SIGSTOP and SIGCONT
void *input_thread_func(void *v){
    state *st=v;
    PTHREAD(pthread_cond_signal(&st->cv2));  //tell the main thread we started
    PTHREAD(pthread_cond_wait(&st->cv1,&st->mtx1)); //wait for the first title to be printed
    char c[32];
    int n=sizeof c;
    while(1){
        memset(c,0,n);
        ATOMIC(SYSCALL(write(STDOUT_FILENO,">> ",3))); //doesnt work with printf
        n=SYSCALL(read(0,c,32));
        if(c[0]=='p'){
            if(st->paused){
                state_play(st);
                ATOMIC(printf("song playing \n"));
            }else{
                state_pause(st);
                ATOMIC(printf("song paused \n"));
            }
        }else if(c[0]=='n'){
            ATOMIC(printf("skipping \n"));
            state_end_song(st);
        }else if(c[0]=='r'){
            ATOMIC(printf("replaying \n"));
            st->song_idx=max(-1,st->song_idx-1);
            state_end_song(st);
        }else if(c[0]=='b'){
            ATOMIC(printf("playing previous song \n"));
            st->song_idx=max(-1,st->song_idx-2);
            state_end_song(st);
        }else if(c[0]=='\n'){
            ATOMIC(printf("\n"));
        }else{
            ATOMIC(printf("invalid command \n"));
        }
    }
}

int main(int argc,char **argv){
    printf("shuffleplay \n");
    printf("usage: ./shuffleplay /path/to/music/directory \n");
    printf("enter p to play/pause \n");
    printf("enter n to go to the next song \n");
    printf("enter b to go back to the previous song \n");
    printf("enter r to restart the current song \n");
    
    //these are for my own personal use
    char *spotify="/Users/apple1/sound/spotify/songs to yeet myself ofa building to";
    char *youtube="/Users/apple1/sound/ytmusic";
    
    char *path="./songs";  //play music in ./songs directory by default
    if(argc>1){
        if     (strcmp(argv[1],"-y")==0) path=youtube;
        else if(strcmp(argv[1],"-s")==0) path=spotify;
        else path=argv[1];
    }

    SYSCALL(chdir(path));
    int num_songs;
    char **songs=getlist(&num_songs); //get song list
    shuffle(songs,num_songs);         //shuffle the list
    
#ifdef DEBUG
    printf("\n");
    for(int i=0;i<num_songs;i++) printf("%s\n",songs[i]);
    printf("\n");
#endif
    
    state st;
    state_init(&st);

    PTHREAD(pthread_mutex_init(&print_mtx,0));
    
    pthread_t thread;
    PTHREAD(pthread_create(&thread,0,input_thread_func,&st)); //create input thread
    PTHREAD(pthread_cond_wait(&st.cv2,&st.mtx2)); //wait for the input thread to start
    
    //play all songs in the shuffled list
    int status=1;
    for(st.song_idx=0;st.song_idx<num_songs;st.song_idx++){
        ATOMIC(printf("\n%s\n",songs[st.song_idx]));
        if(WIFEXITED(status)) ATOMIC(SYSCALL(write(STDOUT_FILENO,">> ",3)));
        PTHREAD(pthread_cond_signal(&st.cv1)); //allow input thread to continue if stopped
        status=play_song(songs[st.song_idx],&st);
    }
    printf("done\n");
}




