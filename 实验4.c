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

/* 约束常量 */
#define MAXLINE    1024   /* 命令行输入的最大长度 */
#define MAXARGS     128   /* 单条命令的参数数量上限 */
#define MAXJOBS      16   /* Shell能同时管理的最大作业数 */
#define MAXJID    1<<16   /* 作业ID（jid）的最大值 */

/* 作业状态 */
#define UNDEF 0 /* 未定义或无效 */
#define FG 1    /* 前台 */
#define BG 2    /* 后台 */
#define ST 3    /* 停止 */

/* 
 * 作业状态: FG (foreground), BG (background), ST (stopped)
 * 作业状态转换相关操作:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * 前台最多只有一个作业运行.
 */

/* 全局变量 */
extern char **environ;      /* 指向环境变量表的指针（定义在libc中） */
char prompt[] = "tsh> ";    /* 命令行提示符（不可更改） */
int verbose = 0;            /* 如果为1，打印调试信息 */
int nextjid = 1;            /* 下一个分配的作业ID（jid） */
char sbuf[MAXLINE];         /* 用于格式化字符串的缓冲区 */

struct job_t {              /* 作业结构体 */
    pid_t pid;              /* 进程ID */
    int jid;                /* 作业ID [1, 2, ...] */
    int state;              /* 作业状态 */
    char cmdline[MAXLINE];  /* 用户输入的命令行字符串 */
};
struct job_t jobs[MAXJOBS]; /* 作业列表 */
/* 全局变量结束 */


/* 函数原型 */

/* 需要自己实现的函数 */
void eval(char *cmdline);        //分析命令，并派生子进程执行，主要功能是解析命令行并运行
int builtin_cmd(char **argv);    //解析和执行bulitin命令，包括 quit, fg, bg, and jobs
void do_bgfg(char **argv);       //执行bg和fg命令
void waitfg(pid_t pid);          //实现阻塞等待前台程序运行结束

void sigchld_handler(int sig);   //SIGCHLD信号处理函数
void sigint_handler(int sig);    //信号处理函数，响应 SIGINT (ctrl-c) 信号 
void sigtstp_handler(int sig);   //信号处理函数，响应 SIGTSTP (ctrl-z) 信号

/* 已经提供的函数 */
int parseline(const char *cmdline, char **argv);                     // 获取参数列表，返回是否为后台运行命令
void sigquit_handler(int sig);                                       // 处理SIGQUIT信号

void clearjob(struct job_t *job);                                    // 清除作业结构体
void initjobs(struct job_t *jobs);                                   // 初始化作业列表
int maxjid(struct job_t *jobs);                                      // 返回作业列表中最大的jid
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline); // 向作业列表中添加一个作业
int deletejob(struct job_t *jobs, pid_t pid);                        // 在作业列表中删除pid对应的作业
pid_t fgpid(struct job_t *jobs);                                     // 返回当前前台运行作业的pid
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);              // 根据pid找到对应的作业
struct job_t *getjobjid(struct job_t *jobs, int jid);                // 根据jid找到对应的作业
int pid2jid(pid_t pid);                                              // 根据pid找到jid
void listjobs(struct job_t *jobs);                                   // 打印作业列表

void usage(void);                                                    // 打印帮助信息
void unix_error(char *msg);                                          // unix错误处理程序
void app_error(char *msg);                                           // app错误处理程序
typedef void handler_t(int);                                         
handler_t *Signal(int signum, handler_t *handler);                   // 为指定的信号设置自定义的信号处理函数

/*
 * main - Shell的主要程序，功能是在文件中逐行获取命令，并调用eval函数进行解析
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE]; /* 存储用户输入的命令行 */
    int emit_prompt = 1; /* 是否显示提示符（默认显示） */

    /* 将 stderr 重定向到 stdout（使所有输出（包括错误）通过 stdout 管道） */
    dup2(1, 2);

    /* 命令行参数解析 */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* 打印帮助信息 */
            usage();
	    break;
        case 'v':             /* 启用详细调试模式（打印调试信息） */
            verbose = 1;
	    break;
        case 'p':             /* 禁用提示符（自动化调试用） */
            emit_prompt = 0;  
	    break;
	    default:              /* 无效选项 */
            usage();
	    }
    }

    /* 信号处理函数（需要自己实现） */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c（中断前台作业） */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z（暂停前台作业） */
    Signal(SIGCHLD, sigchld_handler);  /* 子进程终止或暂停 */

    /* 提供一种干净的方式终止shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* 初始化作业列表 */
    initjobs(jobs);

    /* 执行shell的“读取-解析-执行“循环 */
    while (1) {

        /* 显示提示符 */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }

        /* 读取命令行输入 */
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* 输入结束 (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* 解析并执行命令行 */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - 解析用户输入的命令行内容
 * 
 * 如果用户请求了一个内置命令（quit、jobs、bg或fg），那么立即执行它。否则，
 * 创建子进程并在子进程的上下文中运行作业。如果作业在前台运行，则等待它终止，
 * 然后返回。注意：每个子进程必须有一个唯一的进程组ID，这样当我们在键盘上键入
 * ctrl-c （ctrl-z）时，我们的后台子进程才不会从内核接收SIGINT （SIGTSTP）。
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    pid_t pid;
    int bg = parseline(cmdline, argv); /* 解析命令行，返回给argv数组 */

    if (argv[0] == NULL) /* 命令行为空 */
        return;
    
    /* 如果不是内置命令 */
    if (!builtin_cmd(argv))
    {
        sigset_t set;

        /* 初始化信号集set，并将信号加入set */
        if (sigemptyset(&set) < 0)
            unix_error("sigemptyset error");
        if (sigaddset(&set, SIGINT) < 0 || sigaddset(&set, SIGTSTP) < 0 || sigaddset(&set, SIGCHLD) < 0)
            unix_error("sigaddset error");

        /* 阻塞SIGCHLD信号 */
        if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
            unix_error("sigprocmask error"); 

        /* 创建子进程失败 */
        if((pid = fork()) < 0)
            unix_error("fork error"); 
        
        /* 子进程 */
        else if(pid == 0) 
        {
            /* 恢复受阻塞的信号 */
            if (sigprocmask(SIG_UNBLOCK, &set, NULL) < 0) 
                unix_error("sigprocmask error"); 

            /* 设置子进程为新的进程组组长 */
            if (setpgid(0, 0) < 0) 
                unix_error("setpgid error");
            
            /* 执行子进程 */
            if (execve(argv[0], argv, environ) < 0)
            {
                printf("%s: Command not found\n", argv[0]);
                exit(0);
            }
        }

        addjob(jobs, pid, bg + 1, cmdline); /* 将作业加入到作业列表 */
        
        /* 恢复受阻塞的信号 */
        if (sigprocmask(SIG_UNBLOCK, &set, NULL) < 0) 
                unix_error("sigprocmask error");
                
        /* 前台作业 */
        if (!bg)
            waitfg(pid); /* 等待子进程的前台作业完成 */

        /* 后台作业*/
        else
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); /* 将进程id映射到job id */
    }
    return;
}

/* 
 * parseline - 解析命令行并构建argv数组.
 * 
 * 用单引号括起来的字符被视为一个单一的参数，如果用户请求的是后台进程，则返回真；
 * 否则，返回假。
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* 保存命令行的本地副本 */
    char *buf = array;          /* 遍历命令行的指针 */
    char *delim;                /* 指向第一个空格分隔符的指针 */
    int argc;                   /* 参数数量 */
    int bg;                     /* 后台作业标志 */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* 用空格替换末尾的'\n' */
    while (*buf && (*buf == ' ')) /* 忽略开头的空格 */
	    buf++;

    /* 建立参数列表 */
    argc = 0;
    if (*buf == '\'') { /* 处理单引号包裹的参数 */
        buf++;
        delim = strchr(buf, '\''); /* 有闭合引号，以引号分割 */
    }
    else {
	    delim = strchr(buf, ' '); /*无闭合引号，以空格分隔 */
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* 跳过空格 */
            buf++;
        
        /* 查找下一个分隔符 */
        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* 忽略空行 */
	    return 1;

    /* 判断是否为后台作业 */
    if ((bg = (*argv[argc-1] == '&')) != 0) { /* 检查最后一个参数是否为'&' */
	    argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - 如果用户输入了内置命令，则立即执行该命令  
 */
int builtin_cmd(char **argv) 
{
    if (!strcmp(argv[0], "quit")) /* 如果命令是quit，退出 */
        exit(0);
    else if (!strcmp(argv[0], "jobs")) /* 如果命令是jobs，列出所有正在运行的后台作业 */
        listjobs(jobs);
    else if (!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg")) /* 如果命令是bg或fg，调用do_bgfg函数处理 */
        do_bgfg(argv);
    else
        return 0;     /* 不是内置命令 */
    return 1;
}

/* 
 * do_bgfg - 执行内置命令bg和fg
 */
void do_bgfg(char **argv) 
{
    struct job_t *job;
    char *cmd = argv[0]; /* "bg" 或 "fg" */
    char *arg = argv[1]; /* 参数（如 "%1" 或 "1234"） */
    int jid, pid;

    /* 错误检查：必须有参数 */
    if (arg == NULL) 
    {
        printf("%s command requires PID or %%jobid argument\n", cmd);
        return;
    }

    /* 解析参数类型（jid 或 pid） */
    if (arg[0] == '%') /* 处理作业号（JID），如 "%2" */
    {  
        if (sscanf(&arg[1], "%d", &jid) < 1) /* 从 % 后提取数字到 jid */
        {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]); /* 提取失败 */
            return;
        }
        if ((job = getjobjid(jobs, jid)) == NULL) /* 根据 jid 查找作业 */
        {
            printf("%%%d: No such job\n", jid); /* 作业不存在 */
            return;
        }
    } 
    else /* 处理进程号（PID），如 "1234" */
    {  
        if (sscanf(arg, "%d", &pid) < 1) /* 提取整个参数到 pid */
        {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]); /* 提取失败 */
            return;
        }
        if ((job = getjobpid(jobs, pid)) == NULL) /* 根据 pid 查找作业 */
        {
            printf("(%d): No such process\n", pid); /* 作业不存在 */
            return;
        }
    } 

    /* 发送 SIGCONT 信号到整个进程组，恢复进程 */
    if (kill(-(job->pid), SIGCONT) < 0) 
        unix_error("kill (SIGCONT) error");

    // 更新作业状态
    job->state = (strcmp(cmd, "fg") == 0) ? FG : BG;

    /* 如果是前台作业，需要阻塞等待其完成*/
    if (job->state == FG) 
        waitfg(job->pid);

    /* 如果是后台作业，打印提示信息（与系统 Shell 行为一致） */
    else 
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);

    return;
}

/* 
 * waitfg - 直到进程的标识符不再为前台进程为止
 */
void waitfg(pid_t pid)
{
    sigset_t mask;
    sigemptyset(&mask);

    /* 等待前台作业终止或暂停 */
    while (fgpid(jobs) == pid) {
        sigsuspend(&mask); // 等待信号（SIGCHLD/SIGINT/SIGTSTP）
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - 每当子作业终止（变成僵尸）或因为接收到SIGSTOP或SIGTSTP
 * 信号而停止时，内核都会向shell发送SIGCHLD。处理程序获取所有可用的僵尸子进程，
 * 但不等待任何其他当前正在运行的子进程终止。
 */
void sigchld_handler(int sig)
{
    int status;
    pid_t pid;
    struct job_t *job;
    if (verbose)
        puts("sigchld_handler: entering"); // 输出额外信息

    /* 循环以非阻塞方式回收所有子进程，返回这个子进程的PID，&status中返回其状态 */ 
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) 
    {
        /* 如果当前这个子进程的作业已经删除了，则表示有错误发生 */
        if ((job = getjobpid(jobs, pid)) == NULL)
        {
            printf("Lost track of (%d)\n", pid);
            return;
        }

        int jid = job->jid;
        
        /* 如果子进程通过调用 exit 或者一个返回 (return) 正常终止 */
        if (WIFEXITED(status))
            if (deletejob(jobs, pid))
                if (verbose)
                {
                    printf("sigchld_handler: Job [%d] (%d) deleted\n", jid, pid);
                    /* 用WEXITSTATUS(status) 得到退出编号（exit的参数） */
                    printf("sigchld_handler: Job [%d] (%d) terminates OK (status %d)\n", jid, pid, WEXITSTATUS(status));
                }

        /* 如果子进程是因为一个未被捕获的信号终止的，例如SIGKILL */
        else if (WIFSIGNALED(status))
        {
            if (deletejob(jobs, pid))
                if (verbose)
                    printf("sigchld_handler: Job [%d] (%d) deleted\n", jid, pid);
            /* 使用WTERMSIG (status) 得到使子进程退出的信号编号 */
            printf("Job [%d] (%d) terminated by signal %d\n", jid, pid, WTERMSIG(status));
        }

         /* 如果子进程收到了一个暂停信号（还没退出） */
        else if (WIFSTOPPED(status))
        {
            printf("Job [%d] (%d) stopped by signal %d\n", jid, pid, WSTOPSIG(status));
            /* 使用WSTOPSIG（status）得到使子进程暂停d的信号编号 */
            job->state = ST; /* 状态设为挂起 */
        }
    }

    if (verbose)
        puts("sigchld_handler: exiting");
    return;
}

/* 
 * sigint_handler - 当用户在键盘上键入ctrl-c时，内核向shell发送一个SIGINT
 * 信号。捕获它并将其发送到前台作业。
 */
void sigint_handler(int sig) 
{
    if (verbose)
        puts("sigint_handler: entering");
    /* 获取当前前台作业的 PID */
    pid_t fg_pid = fgpid(jobs);
    
    if (fg_pid > 0) 
    {
        /* 向前台作业发送 SIGINT 信号（等价于用户按 ctrl-c）*/
        if (kill(-fg_pid, SIGINT) < 0)
            unix_error("kill SIGINT error");

        if (verbose)
            printf("sigint_handler: Job (%d) killed\n", fg_pid);
    }

    if (verbose)
        puts("sigint_handler: exiting");
    return;
}

/*
 * sigtstp_handler - 每当用户在键盘上键入ctrl-z时，内核都会向shell发送一个
 * SIGTSTP信号。通过发送SIGTSTP信号，捕获它并挂起前台作业。
 */
void sigtstp_handler(int sig) 
{
    if (verbose)
        puts("sigstp_handler: entering");
    /* 获取当前前台作业的 PID */
    pid_t fg_pid = fgpid(jobs);
    struct job_t *job = getjobpid(jobs, fg_pid);

    if (fg_pid > 0) 
    {
        /* 向前台作业发送 SIGTSTP 信号（等价于用户按 ctrl-z）*/
        if (kill(-fg_pid, SIGTSTP) < 0)
            unix_error("kill SIGSTOP error");

        if (verbose)
            printf("sigstp_handler: Job [%d] (%d) stopped\n", jobs->jid, fg_pid);
    }

    if (verbose)
        puts("sigstp_handler: exiting");
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

/* addjob - 增加一个作业到作业列表 */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    /* 无效pid，直接返回 */
    if (pid < 1)
	    return 0;

    /* 遍历作业列表，寻找空闲位置 */
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;

            /* 超出MAXJOBS，循环使用jid */
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);

            /* 调试模式下打印添加信息 */
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    /* 作业列表已满 */
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
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
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
    {
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
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

