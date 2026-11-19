/* Opt-in launchd task: static USB address and key-only OpenSSH on that address.
 * Files live on the normal HFS /var; no raw storage access. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/sockio.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define PREFIX "/private/var/db/hakios-usb-jb"
#define SOURCE "/redwood/share/usb-jb"
static int mkdir_ok(const char *p, mode_t mode)
{
    struct stat st;
    if (!mkdir(p, mode)) return 0;
    return errno == EEXIST && !lstat(p, &st) && S_ISDIR(st.st_mode) && st.st_uid == 0 ? 0 : -1;
}
/* Graft only our dedicated prefix; etc is writable for pwd_mkdb. File bytes
 * remain in the read-only CA payload. No existing package-manager tree is changed. */
static int graft(const char *relative)
{
    char src[1024], dst[1024]; struct stat st;
    if (snprintf(src,sizeof(src),"%s%s",SOURCE,relative) >= (int)sizeof(src) ||
        snprintf(dst,sizeof(dst),"%s%s",PREFIX,relative) >= (int)sizeof(dst) || lstat(src,&st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir_ok(dst,0755)) return -1;
        DIR *d=opendir(src); if(!d) return -1;
        struct dirent *e; int rc=0;
        while((e=readdir(d))) {
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            char child[1024];
            if(snprintf(child,sizeof(child),"%s/%s",relative,e->d_name)>=(int)sizeof(child)||graft(child)) {rc=-1;break;}
        }
        closedir(d); return rc;
    }
    if (S_ISREG(st.st_mode)) {
        if(!symlink(src,dst)) return 0;
        if(errno!=EEXIST) return -1;
        char target[1024]; ssize_t n=readlink(dst,target,sizeof(target)-1);
        if(n<0) return -1; target[n]=0;
        return strcmp(target,src) ? -1 : 0;
    }
    return -1; /* Build tool materializes package symlinks into ordinary files. */
}
static int text_file(const char *path,const char *text,mode_t mode)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,mode); if(fd<0)return -1;
    size_t n=strlen(text); ssize_t done=write(fd,text,n);
    int rc=done==(ssize_t)n && !fchmod(fd,mode) ? 0 : -1;
    close(fd); return rc;
}
static int run(char *const argv[])
{
    pid_t p=fork(); if(p<0)return -1;
    if(!p) {execv(argv[0],argv);_exit(127);}
    int status; while(waitpid(p,&status,0)<0) {if(errno!=EINTR)return -1;}
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
}
/* The package manager's prefix (Procursus, src/init.c) names sshd's privilege separation user "_sshd",
 * but its sshd asks for "sshd" and exits. Its etc is a real, writable copy (kept across boots on HFS),
 * so add the name there once, uid 75 like _sshd, and rebuild the database libiosexec reads. */
static int sshd_user(void)
{
    static const char line[]="sshd:*:75:75::0:0:SSH privilege separation:/var/jb/var/empty:/usr/bin/false\n";
    char text[16384]; int fd=open("/var/jb/etc/master.passwd",O_RDWR|O_APPEND); if(fd<0)return -1;
    ssize_t n=read(fd,text,sizeof(text)-1); if(n<0){close(fd);return -1;} text[n]=0;
    if(!strncmp(text,"sshd:",5)||strstr(text,"\nsshd:")) {close(fd);return 0;}
    ssize_t done=write(fd,line,sizeof(line)-1); close(fd);
    if(done!=(ssize_t)(sizeof(line)-1))return -1;
    fprintf(stderr,"[usb-network] added the sshd user to /var/jb/etc/master.passwd\n");
    char *args[]={"/var/jb/usr/sbin/pwd_mkdb","-p","/var/jb/etc/master.passwd",NULL};
    return run(args);
}
static int prefix(void)
{
    char target[1024]; struct stat st;
    if(!lstat("/var/jb",&st)) {
        ssize_t n=readlink("/var/jb",target,sizeof(target)-1);
        if(n<0) {fprintf(stderr,"[usb-network] existing /var/jb; using its sshd\n");return sshd_user();}
        target[n]=0;
        if(strcmp(target,PREFIX)) {fprintf(stderr,"[usb-network] existing foreign /var/jb; using its sshd\n");return sshd_user();}
    } else if(errno!=ENOENT || symlink(PREFIX,"/var/jb")) return -1;
    if(graft("") || mkdir_ok(PREFIX "/etc",0755) || mkdir_ok(PREFIX "/var",0755) ||
        mkdir_ok(PREFIX "/var/empty",0755) || mkdir_ok(PREFIX "/var/run",0755)) return -1;
    if(text_file(PREFIX "/etc/master.passwd",
        "root:NP:0:0::0:0:USB administrator:/var/root:/var/jb/bin/sh\n"
        "sshd:*:75:75::0:0:SSH privilege separation:/var/jb/var/empty:/usr/bin/false\n",0600) ||
        text_file(PREFIX "/etc/group","wheel:*:0:root\nsshd:*:75:\n",0644))return -1;
    char *args[]={"/var/jb/usr/sbin/pwd_mkdb","-p","/var/jb/etc/master.passwd",NULL};
    return run(args);
}
static void addr(struct sockaddr_in *s,const char *ip)
{ memset(s,0,sizeof(*s));s->sin_len=sizeof(*s);s->sin_family=AF_INET;inet_pton(AF_INET,ip,&s->sin_addr); }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    char args[1024]={0};size_t n=sizeof(args)-1;int usb=0,ncm=0;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0))return 1;
    for(char *p=strtok(args," ");p;p=strtok(NULL," ")) {usb|=!strcmp(p,"usb=1");ncm|=!strcmp(p,"usb_ncm=1");}
    if(!usb||!ncm)return 0;
    for(unsigned i=0;i<120&&!if_nametoindex("en2");++i)sleep(1);
    if(!if_nametoindex("en2")) {fprintf(stderr,"[usb-network] no en2 after120s\n");return 1;}
    int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0){perror("usb-network socket");return 1;}
    struct ifaliasreq request={0};strlcpy(request.ifra_name,"en2",sizeof(request.ifra_name));
    addr((struct sockaddr_in *)&request.ifra_addr,"172.16.42.1");
    addr((struct sockaddr_in *)&request.ifra_broadaddr,"172.16.42.255");
    addr((struct sockaddr_in *)&request.ifra_mask,"255.255.255.0");
    if(ioctl(fd,SIOCAIFADDR,&request)) {perror("usb-network SIOCAIFADDR");close(fd);return 1;}
    close(fd);printf("[usb-network] en2 static 172.16.42.1/24; no default route\n");
    if(prefix()) {perror("usb-network SSH prefix");return 1;}
    char *argv[]={"/var/jb/usr/sbin/sshd","-D","-e","-f","/redwood/share/usb-ssh/sshd_config",NULL};
    printf("[usb-network] starting key-only sshd on 172.16.42.1:22\n");
    setenv("PATH","/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin:/usr/sbin:/sbin",1);
    execv(argv[0],argv);perror("usb-network sshd");return 1;
}
