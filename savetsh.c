/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name and login ID here>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */
#define MAXHISTORY   10   /* max records of history */ 

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */  //..??
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */  //..
char * username;            /* The name of the user currently logged into the shell */
struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */  // 这个是leader的pid ？ 一个job有多个process怎么存 pid？？
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* where to store the next history record
indicate the position of the oldest command as well (if there is a command at history_idx)*/
int history_idx = 0;        
// 读的时候，从idx开始读，如果idx为空，找到。   存之后，idx++，%10
char history[MAXHISTORY][MAXLINE];  /* the last 10 records of history */
int shell_pid;
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* helper functions */
void init_history();
void add_proc(char * name, pid_t pid, pid_t ppid, char * stat);
pid_t check_suspend();
pid_t check_run();
void remove_proc(pid_t pid);
void do_quit();
void change_proc_stat(pid_t pid, char * stat);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);
char * login();
void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);  //。。？
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);  //??

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {  //?？getopt
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	    default:
            usage();
	    }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);   // 怎么发SIGQUIT信号？

    /* Initialize the job list */
    initjobs(jobs);

    /* Have a user log into the shell */
    username = login();

    // 把shell 创建proc
    shell_pid = getpid();
    add_proc("tsh", shell_pid, getppid(), "Rs+");
    
    /* Init history for the user that has logged in */
    init_history();
    
    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))  //ferror用法？
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }


        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}

void init_history(){  
    char path[MAXLINE];
    sprintf(path, "./home/%s/.tsh_history", username);

    FILE * fp = fopen(path, "r");
    if(fp == NULL){
        unix_error("fopen");
    }

    int i = 0;
    while(fgets(history[i], MAXLINE, fp) != NULL){
        i++;        
    }

    // 最后一行是纯空行时， 是否会被认为是NULL  是
    fclose(fp);

    for(int j = i; j < MAXHISTORY; j++){
        history[j][0] = '\0';
    }

    history_idx = i % MAXHISTORY;
}


// if name exists in file passwd, set variable password to be the corresponding password
int exist_user(char * name, char password[]){ 
    char line[MAXLINE*2];
    char * line_name, * line_password;

    FILE * fp = fopen("./etc/passwd", "r");
    if(fp == NULL){
        unix_error("fopen");
    }

    while(fgets(line, MAXLINE*2, fp)!=NULL){
        line_name = strtok(line, ":");
        if(strcmp(name, line_name) == 0){
            line_password = strtok(NULL, ":");  // 这个password，在返回后依旧还存在吗
            strcpy(password, line_password); 
            fclose(fp);
            return 1;
        }
        
    }

    fclose(fp);
    return 0;
}

int check_auth(char * name, char * passwd){
    char line_password[MAXLINE];
    if(exist_user(name, line_password)){
        if(strcmp(line_password, passwd) == 0){
            return 1;
        }
    }
    return 0;
}

/*
 * login - Performs user authentication for the shell
 *
 * See specificaiton for how this function should act
 *
 * This function returns a string of the username that is logged in
 */
char * login() {
    char * name = (char*)malloc(sizeof(char) * MAXLINE);
    char passwd[MAXLINE];

    while(1){
        printf("username: ");
        fflush(stdout);
        fgets(name, MAXLINE, stdin);    // use fgets, instead of scanf, in case there are spaces in the input
        name[strlen(name) - 1] = '\0';  // deal with '\n' at the end
        if(strcmp(name, "quit")==0){
            free(name);
            exit(0);   // 此时退出，不需要删proc。因为此时还没有为自制shell建立proc文件夹，proc需要直到username. 以及没有其他子进程
        }

        printf("password: ");
        fflush(stdout);
        fgets(passwd, MAXLINE, stdin); 
        passwd[strlen(passwd) - 1] = '\0';
        if(strcmp(passwd, "quit")==0){
            free(name);
            exit(0);
        }

        if(check_auth(name, passwd)){
            return name;
        }else{
            printf("User Authentication failed. Please try again.\n");
        }
    }

}

int history_start(){
    int start = history_idx;
    if(history[start][0] == '\0'){
        start = 0;
    }
    return start;
}

void add_history(char * cmdline){
    strcpy(history[history_idx], cmdline);
    history_idx = (history_idx + 1) % MAXHISTORY;

    // change .tsh_history  如果idx为0，说明对应从0开始到
    char path[MAXLINE];
    sprintf(path, "./home/%s/.tsh_history", username);

    int start = history_start();

    FILE * fp = fopen(path, "w");
    if(fp == NULL){
        unix_error("fopen");
    }

    // history[][]中带\n, cmdline也带\n
    for(int count = 0; count < MAXHISTORY && history[start][0]!='\0'; count++){
        fprintf(fp, "%s", history[start]);
        start = (start + 1) % MAXHISTORY;
    }

    fclose(fp);
}

void list_history(){
    int start = history_start();

    for(int count = 0; count < MAXHISTORY && history[start][0] != '\0'; count++){
        printf("%d %s", count + 1, history[start]);
        start = (start + 1) % MAXHISTORY; 
    }
} 

char * nth_history(int n){
    int start = history_start();
    start = (start + n - 1) % MAXHISTORY;
    return history[start];   //   可能history[start] == '\0'
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    

    char *argv[MAXARGS];
    int bg;
    pid_t pid;

    bg = parseline(cmdline, argv);
    if(argv[0] == NULL){
        return;
    }

    // 如果是NULL 不加
    if(argv[0][0] != '!')
        add_history(cmdline);


    /*builtin, 直接执行。 非builtin，才分bg fg*/
    if(!builtin_cmd(argv)){


        // block all signals  
        // block SIGINT 因为如果处理的话，要从jobs中找fg，而此时可能还没加入jobs
        // block SIGCHLD 因为如果子终止了，从jobs中删，但还没加入jobs
        sigset_t mask_all, prev;
        sigfillset(&mask_all);
        sigprocmask(SIG_BLOCK, &mask_all, &prev);   // block all signals

        if((pid = fork()) ==0){
            // add proc   argv pid,

            sigprocmask(SIG_SETMASK, &prev, NULL);   //子中需要unblock，否则无法处理ctrl z，c的信号
            setpgid(0, 0);          // put child in a new process group
            if(execve(argv[0], argv, environ) < 0){
                printf("%s: Command not found.\n", argv[0]);
                exit(1);
            }
        }

        int state;
        char stat[3];
        if(!bg){
            state = FG;
            strcpy(stat, "R+");
            change_proc_stat(shell_pid, "Ss");
        }else{
            state = BG;
            strcpy(stat, "R");
        }
        addjob(jobs, pid, state, cmdline);
        // printf("after add\n");

        add_proc(argv[0], pid, shell_pid, stat);  // 要在 block信号时做，因为如果子终止 去执行handler handler中会删这个文件。 阻塞信号，确保加了文件之后，才会去执行handler

        // printf("after add job\n");

        sigprocmask(SIG_SETMASK, &prev, NULL);  // unblock

        if(!bg){
            waitfg(pid);   // 如果在调waitfg之前或在waitfg中阻塞信号之前 子变成非FG，也是对的，while时就直接不符合条件了
            change_proc_stat(shell_pid, "Rs+");
        }

    }

    return;
}



void write_proc(char * name, pid_t pid, pid_t ppid, char * stat){
    char path[MAXLINE];
    sprintf(path, "./proc/%d/status", pid);
    FILE * fp = fopen(path, "w");
    if(fp == NULL){
        unix_error("fopen");
    }

    fprintf(fp, "Name: %s\n", name);

    
    fprintf(fp, "Pid: %d\n", pid);
    fprintf(fp, "PPid: %d\n", ppid);
    fprintf(fp, "PGid: %d\n", pid);
    fprintf(fp, "Sid: %d\n", shell_pid);

    fprintf(fp, "STAT: %s\n", stat);

    fprintf(fp, "Username: %s\n", username);
    fclose(fp);
}

void change_proc_stat(pid_t pid, char * stat){
    char path[MAXLINE];
    sprintf(path, "./proc/%d/status", pid);
    FILE * fp = fopen(path, "r+");
    if(fp == NULL){
        unix_error("fopen");
    }

    char str[MAXLINE];

    for(int cnt = 0; cnt < 5; cnt++){
        fgets(str, MAXLINE, fp);
    }
    fprintf(fp, "STAT: %s\n", stat);
    fprintf(fp, "Username: %s\n", username);
    
    fclose(fp);
}

void add_proc(char * name, pid_t pid, pid_t ppid, char * stat){
    char path[MAXLINE];
    sprintf(path, "./proc/%d", pid);
    if(mkdir(path, 0777)!=0){
        unix_error("mkdir error");
    }

    write_proc(name, pid, ppid, stat);

    //ppid shelld pid, sid shell pid,  pgid 自己的pid 
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {  // 如果是bg，最后一个参数是去掉了&的
	argv[--argc] = NULL;
    }
    return bg;
}


void add_user(char ** argv){
    if(strcmp(username, "root")!=0){
        printf("root privileges required to run adduser.\n");
        return;
    }
    if(argv[1] == NULL  || argv[2]==NULL){
        printf("need more arguments\n");
        return;
    } else if(argv[3] != NULL){
        printf("too many arguments\n");
        return;
    }

    char password[MAXLINE];
    if(exist_user(argv[1], password)){
        printf("User already exists\n");
        return;
    }

    FILE * fp = fopen("./etc/passwd", "a");
    if(fp == NULL){
        unix_error("fopen");
    }
    fprintf(fp, "%s:%s:/home/%s\n", argv[1], argv[2], argv[1]);
    fclose(fp);

    char path[MAXLINE+20];
    sprintf(path, "./home/%s", argv[1]);  // 测能否创建文件，不能 y

    if(mkdir(path, 0777)!=0)
        unix_error("mkdir error");

    strcat(path, "/.tsh_history");
    FILE * historyp = fopen(path, "w");   // 文件是否会已存在，不会，因为如果用户已存在的话，不会到这
    if(historyp == NULL){
        unix_error("fopen");
    }
    fclose(historyp); 
}

void nth_cmd(char ** argv){
    char nstr[MAXLINE];
    int i;
    for(i=1 ; argv[0][i]!='\0'; i++){  // 暂时不考虑输入！abc的情况 不考虑了 
        nstr[i-1] = argv[0][i];
    }
    nstr[i-1] = '\0';

    int n = atoi(nstr);
    if(n > MAXHISTORY){
        printf("only support the last %d commands\n", MAXHISTORY);
        return;
    }

    char * cmdline = nth_history(n);

    if(cmdline[0] == '\0'){
        printf("no %dth command yet\n", n);
        return;
    }
    eval(cmdline);
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    //判断是否为builtin 
    if(strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg")==0 ){
        do_bgfg(argv);
       
    }else if(strcmp(argv[0], "jobs") == 0){
        if(argv[1] == NULL)
            listjobs(jobs);
        else
            printf("too many arguments\n");
    }else if(strcmp(argv[0], "adduser")==0){ 
        add_user(argv);
    }else if (strcmp(argv[0], "history") == 0){
        if(argv[1] != NULL){
            printf("too many arguments\n");   
        }
        list_history();
        
    }else if (argv[0][0] == '!' ){
        nth_cmd(argv);

    }else if (strcmp(argv[0], "logout") ==0 ){
        if(check_suspend()){
            printf("There are suspended jobs.\n");
        }else{
            do_quit();
        }
    }else if(strcmp(argv[0], "quit") == 0){
        do_quit();
    }else{
        return 0;
    }



    return 1;

        
}




pid_t check_suspend(){
    int i;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == ST)
            return jobs[i].pid;
    return 0;
}

pid_t check_run(){
    int i;
    printf("checking run\n");
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG || jobs[i].state == BG )
            return jobs[i].pid;
    return 0;
}

void do_quit(){
    free(username);

    for(int i = 0; i < MAXJOBS; i++){
	    if (jobs[i].state == BG || jobs[i].state == FG){
            printf("before kill %d\n",jobs[i].state);
            kill(-jobs[i].pid, SIGINT);
            printf("after kill\n");
            // printf("error: %s", strerror(errno));
            
        }
    }

    printf("here1\n");

    sigset_t mask, prev;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &prev);

    printf("here6\n");

    pid_t p = check_run();
    printf("%d ",p);
    
    while(p!=0){
        printf("%d ",p);
        sigsuspend(&prev);
        p = check_run();
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);

    printf("here7\n");


    remove_proc(shell_pid);
    exit(0);
}

struct job_t * pidjid_str2job(char * str){
    if(str[0] == '%'){
        char jid_str[20];
        int i;
        for(i = 0; str[i+1] != '\0'; i++){
            jid_str[i] = str[i+1];
        }
        jid_str[i] = '\0';
        int jid = atoi(jid_str);
        return getjobjid(jobs, jid);
    }else{
        pid_t pid = atoi(str);  // 参数为char *，  返回值为int 转换为pid_t行不行  测一下。。。
        return getjobpid(jobs, pid);
    }

}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    if(argv[1] == NULL){
        printf("need more arguments\n");
        return;
    }else if(argv[2] != NULL){
        printf("too many arguments\n");
        return;
    }

    sigset_t mask_all, prev;
    sigfillset(&mask_all);
    // sigprocmask(SIG_BLOCK, &mask_all, &prev);

    struct job_t * job = pidjid_str2job(argv[1]);
    if(job == NULL || job->state == UNDEF){
        printf("no such job or process\n");
        return;
    }

    if(strcmp(argv[0], "fg") == 0){
        change_proc_stat(job->pid, "R+");
        change_proc_stat(shell_pid, "Ss");

        if(job->state == ST){  // ST -> FG
            kill(-job->pid, SIGCONT);
        }
        job->state = FG;  // ST -> FG, BG -> FG

        //unblock
        // sigprocmask(SIG_SETMASK, &prev, NULL);

        printf("before waitfg\n");

        waitfg(job->pid);
        change_proc_stat(shell_pid, "Rs+");

    }else if(strcmp(argv[0], "bg") == 0){  // ST -> BG, BG -> BG
        change_proc_stat(job->pid, "R");
        if(job -> state != BG){
            kill(-job->pid, SIGCONT);
        }
        job->state = BG;
        
        //unblock
        // sigprocmask(SIG_SETMASK, &prev, NULL);
    }

    return;
}


/* 
 * waitfg - Block until process pid is no longer the foreground process  ？？等待执行完？或者变成非fg？ 比如stop、（从fg只能变成stop？
 */
void waitfg(pid_t pid)
{
    // printf("in wafg\n");
    sigset_t mask_all, prev;
    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev);

    struct job_t * job = getjobpid(jobs, pid);
    while(job != NULL && job -> state == FG){   // !!注意这里job不为null 可能已删掉 从list中
        sigsuspend(&prev);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);

    // printf("after wafg\n");

    return;
}

void remove_proc(pid_t pid){
    char path[MAXLINE];
    sprintf(path, "./proc/%d/status", pid);
    if(remove(path)!=0){
        unix_error("remove error");
    }

    sprintf(path, "./proc/%d", pid);
    if(rmdir(path)!=0){ // remove directory only if it's empty
        unix_error("rmdir error");
    }

}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
 // child stop,也会发SIGCHLD信号。   不要处理stop了的child
void sigchld_handler(int sig) 
{
    // 要处理所有的zombie， while
    // hints & tips 说只用one call to waitpid，但注释说reap all zombie
    // 如果只是one call，对于bg，比如在sigchld_handler 期间结束的子，无法回收

    int olderrno = errno;
    sigset_t mask_all, prev_all;
    pid_t pid;
    int status;

    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);

    // printf("in chld handler\n");

    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0){

        struct job_t * job = getjobpid(jobs, pid);
        if(WIFSTOPPED(status)){
            job -> state = ST;
        } else if(WIFSIGNALED(status)){
            int signal_num = WTERMSIG(status);
            printf("process %d terminated due to uncaught signal %d: %s\n", job->pid, signal_num, strsignal(signal_num));
            deletejob(jobs, pid);
        } else{
            printf("terminate other\n");
            deletejob(jobs, pid);
        }


        if(WIFSTOPPED(status)){
            change_proc_stat(pid, "T");
        }else{
            remove_proc(pid);
        }


    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);


    // 用不用检查errno的值？  如果发生错误 errno可能是什么值？？
        // Sio_error("waitpid error");   //添加 Sio_error ....  怎么获取errno的信息？？
    errno = olderrno;
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    // 用kill 发  
    // 怎么找到fg job
    int olderrno = errno;

    sigset_t mask_all, prev;
    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev);

    pid_t fg_pid = fgpid(jobs);
    printf("%d\n", fg_pid);

    if(fg_pid!=0)
        kill(-fg_pid, SIGINT);
    
    sigprocmask(SIG_SETMASK, &prev, NULL);  // unblock
    


    // jobs的状态在chldhandler中改，proc也应该在chldhander中改 
   
    errno = olderrno;
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int olderrno = errno;

    sigset_t mask_all, prev;
    sigfillset(&mask_all);
    sigprocmask(SIG_BLOCK, &mask_all, &prev);

    //阻塞所有信号。。。    sigint也是一样。。
    pid_t fg_pid = fgpid(jobs);
    printf("%d\n", fg_pid);
    if(fg_pid!=0)
        kill(-fg_pid, SIGTSTP);

    // jobs的状态在chldhandler中改，proc也应该在chldhander中改 
    sigprocmask(SIG_SETMASK, &prev, NULL);  // unblock
   
    errno = olderrno;
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid > max)
            max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {  // 遍历，找到空位就加入进去。 pid为0表示空闲
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++; //下次直接用nextjid即可。 
            if (nextjid > MAXJOBS)   // 这是不是有问题？？ 用的应该是MAXJID？
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	    return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs)+1;  // 保证了之后加的时候，新的jid比原来最大的jid多1
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;
    printf("in fgpid\n");
    for (i = 0; i < MAXJOBS; i++){
	    if (jobs[i].state == FG)
	        return jobs[i].pid;
    }
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine   何时用unix_error? 有strerror时？
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));  // 把errno对应的字符串信息打印
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.  为何说是child shell?
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



