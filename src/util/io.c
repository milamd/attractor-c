#include "util/io.h"
#include "util/str.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
char *io_read_stream(FILE *stream,size_t limit,size_t *length) {
    if(!stream) {errno=EINVAL;return NULL;}
    StrBuf b; strbuf_init_limit(&b,limit);
    char chunk[8192]; size_t n;
    while(!feof(stream) && !ferror(stream) && (n=fread(chunk,1,sizeof(chunk),stream))>0) {
        strbuf_append(&b,chunk,n);
        if(b.failed) {strbuf_free(&b);return NULL;}
    }
    if(ferror(stream)) {strbuf_free(&b);return NULL;}
    if(length) *length=b.len;
    return strbuf_detach(&b);
}
char *io_read_file(const char *path,size_t limit,size_t *length) {
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    char *data=io_read_stream(f,limit,length);
    if(fclose(f)!=0) {free(data);return NULL;}
    return data;
}
char *io_read_text(const char *path,size_t limit) {
    size_t n=0; char *s=io_read_file(path,limit,&n);
    if(s && memchr(s,0,n)) {free(s);errno=EINVAL;return NULL;}
    return s;
}
bool io_mkdirs(const char *path) {
    if(!path || !*path) {errno=EINVAL;return false;}
    char *p=str_dup(path); if(!p) return false;
    bool ok=true;
    for(char *q=p+1;;q++) {
        if(*q!='/' && *q) continue;
        char c=*q; *q=0;
        if(mkdir(p,0700)!=0) {
            struct stat st;
            if(errno!=EEXIST || lstat(p,&st)!=0 || !S_ISDIR(st.st_mode)) {ok=false;break;}
        }
        *q=c; if(!c) break;
    }
    free(p);return ok;
}

#include <fcntl.h>
#include <unistd.h>
int io_open_workspace(const char *root,const char *path,int flags,unsigned mode,bool contained) {
    if(!root || !path || !*path) {errno=EINVAL;return -1;}
    const char *relative=path;
    if(path[0]=='/') {
        if(contained) {
            size_t n=strlen(root);
            if(strncmp(path,root,n) || (n>1 && path[n]!='/' && path[n]!=0)) {errno=EPERM;return -1;}
            relative=path+n;while(*relative=='/') relative++;
        } else {root="/";while(*relative=='/') relative++;}
    }
    char *copy=str_dup(relative);if(!copy) return -1;
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0) {free(copy);return -1;}
    char *save=NULL,*part=strtok_r(copy,"/",&save);
    while(part) {
        char *next=strtok_r(NULL,"/",&save);
        if(str_eq(part,"..") && contained) {errno=EPERM;goto fail;}
        if(str_eq(part,".")) {part=next;continue;}
        if(!next) {
            int result=openat(fd,part,flags|O_CLOEXEC|O_NOFOLLOW,(mode_t)mode);
            close(fd);free(copy);return result;
        }
        int child=openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(child<0 && errno==ENOENT && (flags&O_CREAT)) {
            if(mkdirat(fd,part,0700)<0 && errno!=EEXIST) goto fail;
            child=openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        }
        if(child<0) goto fail;close(fd);fd=child;part=next;
    }
    if(!(flags&(O_CREAT|O_TRUNC|O_WRONLY|O_RDWR))) {free(copy);return fd;}
    errno=EINVAL;
fail:{int error=errno;close(fd);free(copy);errno=error;return -1;}
}
char *io_artifact_name(const char *id) {
    if(!id || !*id || strlen(id)>100) {errno=EINVAL;return NULL;}
    StrBuf b;strbuf_init(&b);strbuf_append_cstr(&b,"node-");
    for(const unsigned char *p=(const unsigned char *)id;*p;p++) strbuf_appendf(&b,"%02x",(unsigned)*p);
    return strbuf_detach(&b);
}
bool io_artifact_write(const char *root,const char *id,const char *name,const char *content) {
    char *component=io_artifact_name(id);if(!component) return false;
    StrBuf b;strbuf_init(&b);strbuf_appendf(&b,"%s/%s",component,name);free(component);
    char *relative=strbuf_detach(&b);if(!relative) return false;
    if(!io_mkdirs(root)) {free(relative);return false;}
    int fd=io_open_workspace(root,relative,O_WRONLY|O_CREAT|O_TRUNC,0600,true);free(relative);if(fd<0) return false;
    if(fchmod(fd,0600)!=0) {close(fd);return false;}
    const char *cursor=content?content:"";size_t remaining=strlen(cursor);bool ok=true;
    while(remaining) {
        ssize_t n=write(fd,cursor,remaining);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) {ok=false;break;}cursor+=n;remaining-=(size_t)n;
    }
    if(close(fd)<0) ok=false;return ok;
}

#include <uuid/uuid.h>
static _Thread_local IoFault injected_fault=IO_FAULT_NONE;
void io_test_fault(IoFault fault) {injected_fault=fault;}
static bool fault(IoFault expected) {if(injected_fault==expected) {errno=EIO;return true;}return false;}
bool io_atomic_write(const char *path,const char *bytes,IoDurability durability) {
    if(!path || !bytes || durability<IO_ATOMIC || durability>IO_FULL_SYNC) {errno=EINVAL;return false;}
    char *copy=str_dup(path);if(!copy) return false;
    char *slash=strrchr(copy,'/');const char *directory=".",*name=copy;
    if(slash) {*slash=0;directory=*copy?copy:"/";name=slash+1;}
    if(!*name || !strcmp(name,".") || !strcmp(name,"..")) {free(copy);errno=EINVAL;return false;}
    int dir=io_open_workspace(directory[0]=='/'?"/":".",directory,O_RDONLY|O_DIRECTORY,0,true);if(dir<0) {free(copy);return false;}
    uuid_t uuid;uuid_generate(uuid);char id[37];uuid_unparse_lower(uuid,id);
    char temporary[64];snprintf(temporary,sizeof(temporary),".checkpoint-%s.tmp",id);
    int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) {close(dir);free(copy);return false;}
    FILE *f=fdopen(fd,"w");bool ok=false;
    if(!f) {close(fd);goto cleanup;}
    if(fault(IO_FAULT_WRITE) || fputs(bytes,f)<0) goto close_stream;
    if(fault(IO_FAULT_FLUSH) || fflush(f)!=0) goto close_stream;
    if(durability!=IO_ATOMIC) {
        if(fault(IO_FAULT_SYNC) || fsync(fd)!=0) goto close_stream;
        if(durability==IO_FULL_SYNC) {
#ifdef F_FULLFSYNC
            if(fcntl(fd,F_FULLFSYNC)!=0) goto close_stream;
#else
            errno=ENOTSUP;goto close_stream;
#endif
        }
    }
    if(fclose(f)!=0) {f=NULL;goto cleanup;}f=NULL;
    if(fault(IO_FAULT_REPLACE) || renameat(dir,temporary,dir,name)!=0) goto cleanup;
    ok=durability==IO_ATOMIC || fsync(dir)==0;
    goto cleanup;
close_stream:if(fclose(f)!=0) {}f=NULL;
cleanup:
    {int error=errno;unlinkat(dir,temporary,0);close(dir);free(copy);errno=error;return ok;}
}
