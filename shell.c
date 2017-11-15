/* 
 * tsh - A tiny shell program with job control
 * 
 * <zining_wen zwen6>
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

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

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
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
  pid_t pid;                /* job PID */
  pid_t pgid;               /* process group id*/
  int jid;                  /* job ID [1, 2, ...] */
  int state;                /* UNDEF, BG, FG, or ST */
  char cmdline[MAXLINE];    /* command line */
};

typedef struct command_t {
  char * command;
  struct command_t * next;
}command_t;

struct job_t jobs[MAXJOBS]; /* The job list */
int breakpipe = 0;    //use to stop pipeline
pid_t pgid;
int callstp = 0;
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

void addpipe(command_t *head, char* command);
void do_pipe(command_t *head, int pipecount, char * cmdline);
/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv, int *argc); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid,pid_t pgid,int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
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
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
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
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
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
  char * cmdcopy;
  cmdcopy = malloc((strlen(cmdline)+1) *sizeof(char));
  strcpy(cmdcopy,cmdline);
  pid_t pid = 0;
  char * argv[MAXARGS];
  int bgcheck;
  int argc;
  command_t * head = NULL;
  char * pipeline;
  head = malloc(sizeof(command_t));
  int pipecount = 0;
  //sperate pipe connection
  while ((pipeline = strtok_r(cmdline,"|",&cmdline))){
    if (pipecount == 0){
      head->command = malloc((strlen(pipeline) + 1)*sizeof(char));
      strcpy(head->command,pipeline);
      head->next = NULL;
    }else{
      addpipe(head, pipeline); //add to linklist
    }
    pipecount++;   
  }
  //if not pipeline
  if (pipecount == 1){
    bgcheck = parseline(head->command, argv , &argc);
    if(argc == 0){
      return; 
    }else if (!builtin_cmd(argv)){  //check if builtin command; also execute it
    
      if((pid=fork())<0){   //fork a child
	fprintf(stderr,"fork error.\n");  //print error
      }else if (pid == 0){
	if(execvp(argv[0],argv)<0){
	  fprintf(stderr,"Command not found.\n");//command not found error
	  exit(1);
	}

      }else{
	setpgid(pid,pid);
	if(!bgcheck){	
	  addjob(jobs,pid, getpgid(pid), FG, head->command);//add to job list
	  waitfg(getpgid(pid));
	}else{
	  addjob(jobs,pid, getpgid(pid), BG, head->command);
	  printf("Background <%d>: %s", pid2jid(pid),head->command);
	}
      }
    }
  }else{ //if pipeline
    do_pipe(head, pipecount,cmdcopy);
  }
  return;
}

/*
 * addpipe - Help to build a linklist store pipeline command.
 */
void addpipe(command_t * head,char *command){
  command_t * current = head;
  while (current-> next != NULL){
    current = current->next;
  }
  current->next = malloc(sizeof(command_t));
  current->next->command = malloc((strlen(command) + 1)*sizeof(char));
  strcpy(current->next->command,command);
  current->next->next = NULL;
}

/*
 * do_pipe - Implement pipeline use this function.
 */
void do_pipe(command_t *head, int pipecount, char * cmdline){
  int pipefd[2];
  int fd_in = 0;
  int count = 0;
  pid_t pid;
  char * argv[MAXARGS];
  int argc;
  int bgcheck;
  int dobg = 0;
  command_t *freenode  = NULL; // use this to free command node
  breakpipe = 0;
  for(count = 0;count < pipecount;count++){
    if(breakpipe){
      return;
    }
    //check if running in background
    bgcheck = parseline(head->command,argv,&argc);
    if ((count == (pipecount-1)) && bgcheck)
      dobg = 1;
    pipe(pipefd);
    //fork error, close before return
    if((pid = fork()) < 0){
      fprintf(stderr,"fork error.\n");
      if (count < (pipecount - 1)){
	  close(pipefd[1]);	
      }
      return;
    }
    else if(pid == 0){//child
      if(count == 0){
	setpgid(0,pid); //set pipeline group id as the first child pid
      }else{
	setpgid(0,pgid);
      }
      dup2(fd_in,0);
      if (count < (pipecount - 1))
	dup2(pipefd[1],1);
      close(pipefd[0]);  
      if(execvp(argv[0],argv) < 0){
	fprintf(stderr,"Command in the pipeline not found %s.\n",argv[0]);
	exit(1);
      }
    }else{ //parent
      if(count == 0){
	pgid = pid;
	addjob(jobs,pid,pid,FG,cmdline);
	waitfg(pid);
      }
      else if(count < (pipecount - 1)){
	addjob(jobs,pid,pgid,FG,cmdline);
	waitfg(pid);
      }
      else{
	if(dobg){
	  addjob(jobs,pid,pgid, BG, cmdline);
	}
	else{
	  addjob(jobs,pid,pgid, FG,cmdline);
	  waitfg(pid);
	}
      }
      close(pipefd[1]);
      fd_in = pipefd[0];//move to next pipe
      freenode = head;
      head = head->next;
      //free unneed command
      free(freenode->command);
      free(freenode);
    }
  }
  return;
}


/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv, int * argc) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    //int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    *argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
      argv[(*argc)++] = buf;
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
    argv[*argc] = NULL;
    
    if (*argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[*argc-1] == '&')) != 0) {
      argv[--(*argc)] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
  if (!strcmp(argv[0],"exit")){
    exit(0);
  }else if(!strcmp(argv[0],"cd")){
    if(argv[1] == NULL){
      fprintf(stderr,"cd missing dirctory.\n");

    }else{
      if (chdir(argv[1]) != 0){
	fprintf(stderr,"Wrong dirctory.\n");

      }
    }
    return 1;
  }else if(!strcmp(argv[0],"jobs")){
    listjobs(jobs);
    return 1;
  }else if (!strcmp(argv[0],"fg")||!strcmp(argv[0],"bg")){
    if (argv[1] == NULL){
      fprintf(stderr,"Missing jobs want to be moved.\n");
      return 1;
    }
    do_bgfg(argv);
    return 1;
  }
  return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)  
{
  int jid =  atoi(argv[1]);
  struct job_t *job;
  if (!(job = getjobjid(jobs,jid))){
    fprintf(stderr, "Job id is incorrect.\n");
    return;
  }
  if(!strcmp(argv[0], "fg")){
    if (job->state == ST)
      killpg(getpgid(job->pid),SIGCONT);
    job->state = FG;
    waitfg(job->pid);
  }else if(!strcmp(argv[0] ,"bg")){
    killpg(getpgid(job->pid),SIGCONT);
    job->state = BG;
    printf("Background <%d>: %s", job->jid,job->cmdline);
  } 
  return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
  pid_t fgp;
  fgp = fgpid(jobs);
  struct job_t *job = getjobpid(jobs,pid);
  while(pid == fgp && job!=NULL && job->state == FG){
     sleep(1);
  }
  return;
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
void sigchld_handler(int sig) 
{  
  struct job_t *job;
  int status;
  pid_t waitp;
  pid_t waitgp;
  while((waitp = waitpid(-1,&status,WUNTRACED|WNOHANG))>0) 
    {
      job = getjobpid(jobs,waitp);
      waitgp = job->pgid;
      if(WIFEXITED(status)){
	deletejob(jobs,waitgp);
      }else if(WIFSTOPPED(status)) {
	if(!callstp){
	//	printf("callstop %d\n",callstp);
	//printf("inhere1 %d\n",kill(waitp,SIGCONT));
	  if(kill(waitp,SIGCONT) <0)
	    unix_error("kill");
	}
	else{
	  job->state = ST;
	  breakpipe = 1;
	  callstp = 0;
	}
	//printf("This process was stopped: %sJobid:[%d] Processid:[%d]\n",
	//     job->cmdline, pid2jid(waitp),waitp);
      }else if(WIFSIGNALED(status)){
	//printf("This process was terminated: %sJobid:[%d] Processid:[%d]\n",
	//     job->cmdline, pid2jid(waitp),waitp);
	deletejob(jobs,waitgp);
	breakpipe = 1;
      }      		
    }
  return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{ 
  pid_t fgp;
  if ((fgp=fgpid(jobs)) != 0){
    struct job_t *job = getjobpid(jobs,fgp);
    killpg(job->pgid,SIGINT);     
  }
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
  pid_t fgp;
  if((fgp=fgpid(jobs)) != 0)
    {
      callstp = 1;
      printf("here\n");
      struct job_t *job = getjobpid(jobs,fgp);
      killpg(job->pgid,SIGTSTP);     
    }
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
    job->pgid = 0;
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
int addjob(struct job_t *jobs, pid_t pid, pid_t pgid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].pgid = pgid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
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
    int del = 0;
    if (pid < 1){
	return 0;
    }

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pgid == pid) {
	  clearjob(&jobs[i]);
	  nextjid = maxjid(jobs)+1;
	  del = 1;
	}
    }
    return del;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
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
	    printf("%d: ", jobs[i].jid);
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
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
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
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
