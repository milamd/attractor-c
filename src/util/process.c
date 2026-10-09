#include "util/process.h"
#include "util/str.h"
#include "util/mem.h"
#include "util/io.h"
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#ifdef __APPLE__
#include <Availability.h>
#endif
static long long monotonic_ms(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)!=0) return -1;
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int spawn_chdir(posix_spawn_file_actions_t *actions,const char *directory) {
#ifdef __APPLE__
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if(__builtin_available(macOS 26.0,*)) return posix_spawn_file_actions_addchdir(actions,directory);
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return posix_spawn_file_actions_addchdir_np(actions,directory);
#pragma clang diagnostic pop
#else
    (void)actions;(void)directory;return ENOTSUP;
#endif
}
static void close_fd(int *fd) {if(*fd>=0) {close(*fd);*fd=-1;}}
static char **child_environment(const char *const *extra) {
    const char *names[]={"PATH","HOME","TMPDIR","LANG","LC_ALL","LC_CTYPE","LC_NUMERIC","TZ","USER","LOGNAME","TERM","SHELL"};
    size_t count=0;if(extra) while(extra[count]) {if(count>=128) return NULL;count++;}
    char **env=mem_calloc(count+sizeof(names)/sizeof(*names)+2,sizeof(*env));if(!env) return NULL;
    size_t n=0;
    for(size_t i=0;i<sizeof(names)/sizeof(*names);i++) {
        const char *value=getenv(names[i]);if(!value && i==0) value="/usr/bin:/bin:/usr/sbin:/sbin";
        if(!value) continue;
        StrBuf b;strbuf_init(&b);strbuf_appendf(&b,"%s=%s",names[i],value);env[n]=strbuf_detach(&b);
        if(!env[n]) goto fail;n++;
    }
    for(size_t i=0;i<count;i++) {
        if(!strchr(extra[i],'=') || extra[i][0]=='=') goto fail;
        env[n]=str_dup(extra[i]);if(!env[n]) goto fail;n++;
    }return env;
fail:for(size_t i=0;i<n;i++) free(env[i]);free(env);return NULL;
}
static void drain(int *fd,StrBuf *output,size_t limit,bool *limited) {
    char chunk[8192];
    for(;;) {
        ssize_t n=read(*fd,chunk,sizeof(chunk));
        if(n>0) {
            size_t take=(size_t)n;if(take>limit-output->len) {take=limit-output->len;*limited=true;}
            strbuf_append(output,chunk,take);if(output->failed) {*limited=true;return;}
        } else if(n==0) {close_fd(fd);return;}
        else if(errno==EINTR) continue;
        else if(errno==EAGAIN || errno==EWOULDBLOCK) return;
        else {close_fd(fd);return;}
        /* Return to deadline checks even for a continuously writing child. */
        return;
    }
}
ExecResult *process_run(const ProcessOptions *o) {
    ExecResult *r=mem_calloc(1,sizeof(*r));if(!r) return NULL;r->exit_code=-1;
    if(!o || !o->argv || !o->argv[0] || o->argv[0][0]!='/' || o->timeout_ms<0) {
        r->stderr_buf=str_dup("Invalid process options");r->stdout_buf=str_dup("");return r;
    }
    if(o->cancel && *o->cancel) {r->cancelled=true;r->stdout_buf=str_dup("");r->stderr_buf=str_dup("Cancelled");return r;}
    long long begin=monotonic_ms();if(begin<0) return r;
    long long deadline=begin+(o->timeout_ms?o->timeout_ms:10000);
    size_t limit=o->output_limit?o->output_limit:ATTRACTOR_OUTPUT_LIMIT;
    StrBuf out,err;strbuf_init_limit(&out,limit);strbuf_init_limit(&err,limit);
    int pipes[2][2]={{-1,-1},{-1,-1}};int failure=0;pid_t pid=-1;
    bool reaped=false;int status=0;
    posix_spawn_file_actions_t actions;posix_spawnattr_t attr;bool have_actions=false,have_attr=false;
    char **env=child_environment(o->extra_env);if(!env) {failure=ENOMEM;goto cleanup;}
    for(size_t i=0;i<2;i++) {
        if(pipe(pipes[i])!=0) {failure=errno;goto cleanup;}
        for(size_t j=0;j<2;j++) if(fcntl(pipes[i][j],F_SETFD,FD_CLOEXEC)<0) {failure=errno;goto cleanup;}
        int flags=fcntl(pipes[i][0],F_GETFL);if(flags<0 || fcntl(pipes[i][0],F_SETFL,flags|O_NONBLOCK)<0) {failure=errno;goto cleanup;}
    }
    failure=posix_spawn_file_actions_init(&actions);if(failure) goto cleanup;have_actions=true;
    failure=posix_spawnattr_init(&attr);if(failure) goto cleanup;have_attr=true;
#define ACTION(call) do {failure=(call);if(failure) goto cleanup;} while(0)
    ACTION(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
    ACTION(posix_spawn_file_actions_adddup2(&actions,pipes[0][1],STDOUT_FILENO));
    ACTION(posix_spawn_file_actions_adddup2(&actions,pipes[1][1],STDERR_FILENO));
    for(size_t i=0;i<2;i++) for(size_t j=0;j<2;j++) ACTION(posix_spawn_file_actions_addclose(&actions,pipes[i][j]));
    if(o->working_dir) ACTION(spawn_chdir(&actions,o->working_dir));
    short flags=POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags|=POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    ACTION(posix_spawnattr_setflags(&attr,flags));ACTION(posix_spawnattr_setpgroup(&attr,0));
    sigset_t mask,defaults;sigemptyset(&mask);sigemptyset(&defaults);
    sigaddset(&defaults,SIGPIPE);sigaddset(&defaults,SIGINT);sigaddset(&defaults,SIGTERM);
    ACTION(posix_spawnattr_setsigmask(&attr,&mask));ACTION(posix_spawnattr_setsigdefault(&attr,&defaults));
    ACTION(posix_spawn(&pid,o->argv[0],&actions,&attr,(char *const *)o->argv,env));
#undef ACTION
    close_fd(&pipes[0][1]);close_fd(&pipes[1][1]);
    while(!reaped || pipes[0][0]>=0 || pipes[1][0]>=0) {
        long long now=monotonic_ms();
        if(now<0 || now>=deadline || (o->cancel && *o->cancel) || out.failed || err.failed) {
            r->cancelled=o->cancel && *o->cancel;r->timed_out=now>=deadline;
            kill(-pid,SIGTERM);
            struct timespec grace={0,100000000};while(nanosleep(&grace,&grace)<0 && errno==EINTR) {}
            kill(-pid,SIGKILL);
            if(!reaped) {pid_t waited;do {waited=waitpid(pid,&status,0);} while(waited<0 && errno==EINTR);if(waited==pid) reaped=true;else failure=errno;}
            break;
        }
        struct pollfd fds[2]={{pipes[0][0],POLLIN,0},{pipes[1][0],POLLIN,0}};
        int remaining=(int)(deadline-now);if(remaining>20) remaining=20;
        int polled=poll(fds,2,remaining);
        if(polled<0 && errno!=EINTR) {failure=errno;kill(-pid,SIGKILL);break;}
        if(pipes[0][0]>=0 && fds[0].revents) drain(&pipes[0][0],&out,limit,&r->output_limited);
        if(pipes[1][0]>=0 && fds[1].revents) drain(&pipes[1][0],&err,limit,&r->output_limited);
        if(!reaped) {
            pid_t waited=waitpid(pid,&status,WNOHANG);
            if(waited==pid) reaped=true;
            else if(waited<0 && errno!=EINTR) {failure=errno;kill(-pid,SIGKILL);break;}
        }
    }
    if(!reaped && !failure) {pid_t waited;do {waited=waitpid(pid,&status,0);} while(waited<0 && errno==EINTR);if(waited!=pid) failure=errno;}
    if(!failure) r->exit_code=WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):-1;
cleanup:
    if(pid>0 && !reaped) {
        kill(-pid,SIGKILL);
        pid_t waited;do {waited=waitpid(pid,&status,0);} while(waited<0 && errno==EINTR);
    }
    for(size_t i=0;i<2;i++) for(size_t j=0;j<2;j++) close_fd(&pipes[i][j]);
    if(have_actions) posix_spawn_file_actions_destroy(&actions);if(have_attr) posix_spawnattr_destroy(&attr);
    if(env) {for(size_t i=0;env[i];i++) free(env[i]);free(env);}
    if(failure) strbuf_appendf(&err,"Process error: %s",strerror(failure));
    r->stdout_buf=strbuf_detach(&out);r->stderr_buf=strbuf_detach(&err);
    if(!r->stdout_buf || !r->stderr_buf) {r->exit_code=-1;r->output_limited=true;}
    long long duration=monotonic_ms()-begin;r->duration_ms=duration>INT_MAX?INT_MAX:duration>0?(int)duration:0;
    return r;
}
void exec_result_free(ExecResult *r) {if(r) {free(r->stdout_buf);free(r->stderr_buf);free(r);}}
