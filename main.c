#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ---------- Data model & linked list ----------

typedef struct {
    int id;
    time_t due;
    char description[256];
} Task;

typedef struct Node {
    Task task;
    struct Node *next;
} Node;

static Node *head = NULL;      
static Node *trash_head = NULL;
static int nextId = 1;
static char active_file[512] = "tasks.txt";
static char removed_file[512] = "removed.txt";

static bool use_ansi(void) {
    const char *u = getenv("USE_COLOR");
    return !u || strcmp(u, "0") != 0;
}
static const char *S_RESET(void){ return use_ansi() ? "\x1b[0m"  : ""; }
static const char *S_BOLD(void) { return use_ansi() ? "\x1b[1m"  : ""; }

static const char *C_BLUE(void)  { return use_ansi() ? "\x1b[34m" : ""; }
static const char *C_BLACK(void) { return use_ansi() ? "\x1b[30m" : ""; }
static const char *C_RED(void)   { return use_ansi() ? "\x1b[31m" : ""; }

static int parseInt(const char *s, int *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (*end != '\0') return -1;
    *out = (int)v;
    return 0;
}

static size_t env_limit(const char *name, size_t def){
    const char *s = getenv(name);
    if (!s || !*s) return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v <= 0) return def;
    return (size_t)v;
}

static char *lcase(char *s) {
    for (char *p=s; *p; ++p) *p=(char)tolower((unsigned char)*p);
    return s;
}

static void init_paths(void) {
    const char *a = getenv("CLITASK_FILE");
    const char *r = getenv("CLITASK_REMOVED");
    if (a && *a) {
        strncpy(active_file, a, sizeof active_file - 1);
        active_file[sizeof active_file - 1] = '\0';
    }
    if (r && *r) {
        strncpy(removed_file, r, sizeof removed_file - 1);
        removed_file[sizeof removed_file - 1] = '\0';
    }
}

// ---------- Date & time parsing ----------

static bool parse_mmdd(const char *tok, int *m, int *d) {
    int mm=0, dd=0;
    if (sscanf(tok, "%d/%d", &mm, &dd)==2) {
        if (mm>=1 && mm<=12 && dd>=1 && dd<=31) {
            *m=mm;
            *d=dd;
            return true;
        }
    }
    return false;
}

static bool parse_time_token(const char *tok, int *out_h, int *out_min) {
    char s[32];
    strncpy(s, tok, sizeof s - 1);
    s[sizeof s - 1]='\0';
    lcase(s);
    int h=0, m=0;
    bool pm=false, am=false;
    size_t L=strlen(s);
    if (L>=2 && strcmp(s+L-2,"pm")==0){
        pm=true; s[L-2]='\0';
    } else if (L>=2 && strcmp(s+L-2,"am")==0){
        am=true; s[L-2]='\0';
    }
    if (sscanf(s, "%d:%d", &h, &m) != 2) {
        if (sscanf(s, "%d", &h)!=1) return false;
        m=0;
    }
    if (pm||am) {
        if (h<1||h>12) return false;
        if (pm && h!=12) h+=12;
        if (am && h==12) h=0;
    }
    if (h<0||h>23||m<0||m>59) return false;
    *out_h=h; *out_min=m;
    return true;
}

static time_t make_time_local(int Y,int M,int D,int h,int m){
    struct tm tm={0};
    tm.tm_year=Y-1900;
    tm.tm_mon=M-1;
    tm.tm_mday=D;
    tm.tm_hour=h;
    tm.tm_min=m;
    tm.tm_isdst=-1;
    return mktime(&tm);
}
static void today_YMD(int *Y,int *M,int *D,int *w){
    time_t now=time(NULL);
    struct tm tm;
    localtime_r(&now,&tm);
    if(Y)*Y=tm.tm_year+1900;
    if(M)*M=tm.tm_mon+1;
    if(D)*D=tm.tm_mday;
    if(w)*w=tm.tm_wday;
}

static bool same_ymd(time_t a, time_t b){
    if(!a||!b) return false;
    struct tm ta,tb;
    localtime_r(&a,&ta);
    localtime_r(&b,&tb);
    return ta.tm_year==tb.tm_year &&
           ta.tm_mon==tb.tm_mon &&
           ta.tm_mday==tb.tm_mday;
}

static void fmt_when(time_t t, char *out, size_t L) {
    if (!t) {
        snprintf(out,L,"-");
        return;
    }
    struct tm tm;
    localtime_r(&t,&tm);
    strftime(out,L,"%Y-%m-%d %H:%M",&tm);
}

static time_t parse_due(const char *date_tok, const char *time_tok) {
    int Y,M,D,w;
    today_YMD(&Y,&M,&D,&w);
    int outM=M, outD=D;
    bool have_date=false;
    if (date_tok && *date_tok) {
        char buf[32];
        strncpy(buf,date_tok,sizeof buf - 1);
        buf[sizeof buf - 1]='\0';
        lcase(buf);
        if (strcmp(buf,"today")==0) {
            have_date=true;
        } else if (strcmp(buf,"tomorrow")==0) {
            time_t t=make_time_local(Y,M,D,0,0)+24*3600;
            struct tm tm;
            localtime_r(&t,&tm);
            Y=tm.tm_year+1900;
            outM=tm.tm_mon+1;
            outD=tm.tm_mday;
            have_date=true;
        } else if (parse_mmdd(buf,&outM,&outD)) {
            have_date=true;
            time_t cand=make_time_local(Y,outM,outD,23,59);
            if (difftime(cand,time(NULL))<0) Y+=1;
        }
    }
    int hh=23, mm=59;
    if (time_tok && *time_tok) {
        if (!parse_time_token(time_tok,&hh,&mm)) return 0;
    } else if (!have_date) {
        return 0;
    }
    return make_time_local(Y,outM,outD,hh,mm);
}

static void list_push_head(Node **h, Task t){
    Node *n=(Node*)malloc(sizeof *n);
    if(!n){perror("malloc"); exit(1);}
    n->task=t;
    n->next=*h;
    *h=n;
}

static bool list_remove_by_id(Node **h,int id, Task *out){
    Node *cur=*h,*prev=NULL;
    while(cur){
        if(cur->task.id==id){
            if(prev)prev->next=cur->next;
            else *h=cur->next;
            if(out)*out=cur->task;
            free(cur);
            return true;
        }
        prev=cur;
        cur=cur->next;
    }
    return false;
}

static size_t list_count(Node *h){
    size_t c=0;
    for(;h;h=h->next) c++;
    return c;
}

static void list_free(Node **h){
    Node *c=*h;
    while(c){Node *n=c; c=c->next; free(n);}
    *h=NULL;
}

// ---------- Persistence & storage ----------

static bool load_file(const char *path, Node **out_head, int *io_nextId) {
    *out_head = NULL;
    Node *tail=NULL;
    FILE *f=fopen(path,"r");
    if(!f) return true;
    char line[1024];
    while (fgets(line,sizeof line,f)) {
        int id=0;
        long long due=0;
        char desc[256]={0};
        int n = sscanf(line,"%d %lld %255[^\n]", &id,&due,desc);
        if (n < 3) continue;
        Task t={0};
        t.id=id;
        t.due=(time_t)due;
        strncpy(t.description,desc,sizeof t.description - 1);
        Node *node=(Node*)malloc(sizeof *node);
        if(!node){perror("malloc"); fclose(f); exit(1);}
        node->task=t;
        node->next=NULL;
        if(!*out_head) {
            *out_head=node; tail=node;
        } else {
            tail->next=node; tail=node;
        }
        if (io_nextId && id >= *io_nextId) *io_nextId = id + 1;
    }
    fclose(f);
    return true;
}

static bool save_file(const char *path, Node *h, bool verbose){
    FILE *f=fopen(path,"w");
    if(!f){perror("open for write"); return false;}
    for(Node *n=h;n;n=n->next){
        fprintf(f,"%d %lld %s\n", n->task.id,
                (long long)n->task.due, n->task.description);
    }
    fclose(f);
    if(verbose) printf("Saved %s\n", path);
    return true;
}

static void save_all_quiet(void){
    save_file(active_file, head, false);
    save_file(removed_file, trash_head, false);
}

static void save_all_verbose(void){
    save_file(active_file, head, true);
    save_file(removed_file, trash_head, true);
}

static void reload_all_from_disk(void){
    list_free(&head);
    list_free(&trash_head);
    nextId = 1;
    load_file(active_file, &head, &nextId);
    load_file(removed_file, &trash_head, &nextId);
}

static void print_welcome_header(void){
    const char *B=S_BOLD(), *R=S_RESET(), *BL=C_BLUE();
    printf("%s%s========================================%s\n", BL,B,R);
    printf("%s TODO LIST%s\n", B,R);
    printf("%s========================================%s\n", BL,B?R:"");
    time_t now=time(NULL);
    char buf[64];
    struct tm tm;
    localtime_r(&now,&tm);
    strftime(buf,sizeof buf,"%A, %Y-%m-%d %H:%M",&tm);
    printf("Now: %s\n", buf);
    printf("Active: %s\n", active_file);
    printf("Removed: %s\n\n", removed_file);
}

static bool is_today_local(time_t t){
    if(!t) return false;
    time_t now=time(NULL);
    return same_ymd(t, now);
}

static bool is_tomorrow_local(time_t t){
    if (!t) return false;
    time_t now=time(NULL);
    struct tm tm;
    localtime_r(&now,&tm);
    tm.tm_mday += 1;
    time_t tom = mktime(&tm);
    return same_ymd(t, tom);
}

static int cmp_task_ptrs(const void *a,const void *b){
    const Node *na=*(const Node * const *)a, *nb=*(const Node * const *)b;
    if (na->task.due==0 && nb->task.due==0) return na->task.id - nb->task.id;
    if (na->task.due==0) return 1;
    if (nb->task.due==0) return -1;
    if (na->task.due < nb->task.due) return -1;
    if (na->task.due > nb->task.due) return 1;
    return na->task.id - nb->task.id;
}

static void print_task_row(const Task *t){
    char when[32];
    fmt_when(t->due, when, sizeof when);
    printf("%-16s %-3d %s\n", when, t->id, t->description);
}

static Node **collect_sorted(Node *h, size_t *out_n){
    size_t n=list_count(h);
    Node **arr=(Node**)malloc(n ? n * sizeof *arr : sizeof *arr);
    if (!arr) { perror("malloc"); exit(1); }
    size_t i=0;
    for(Node *c=h;c;c=c->next) arr[i++]=c;
    qsort(arr,n,sizeof(Node*),cmp_task_ptrs);
    *out_n = n;
    return arr;
}

// ---------- Commands, HTTP server & watcher ----------

static void cmd_help(int argc, char **argv){
    (void)argc; (void)argv;
    print_welcome_header();
    printf("Commands:\n");
    printf(" add \"desc\" [date] [time]\n");
    printf(" list\n");
    printf(" delete <id>\n");
    printf(" removed\n");
    printf(" save\n");
    printf(" help\n");
    printf(" serve <port> # view tasks via HTTP at /\n");
    printf(" watch [interval] [lead_min] [notify-cmd ...]\n");
    printf("\n");
}

static void cmd_add(int argc, char **argv){
    if (argc < 1) {
        printf("Usage: add \"desc\" [date] [time]\n");
        return;
    }
    const char *desc_in = argv[0];
    const char *date_tok = (argc>=2)? argv[1] : NULL;
    const char *time_tok = (argc>=3)? argv[2] : NULL;
    char desc[256];
    strncpy(desc, desc_in, sizeof desc - 1);
    desc[sizeof desc - 1]='\0';
    time_t due = parse_due(date_tok, time_tok);
    Task t={0};
    t.id=nextId++;
    t.due=due;
    strncpy(t.description, desc, sizeof t.description - 1);
    list_push_head(&head, t);
    char when[32];
    fmt_when(t.due, when, sizeof when);
    printf("%sAdded%s #%d: %s (due: %s)\n",
           C_BLUE(), S_RESET(), t.id, t.description, when);
    save_all_quiet();
}

static void cmd_list(int argc, char **argv){
    (void)argc; (void)argv;
    print_welcome_header();
    size_t n=list_count(head);
    if (n==0) {
        printf("No tasks.\n");
        return;
    }
    Node **arr=collect_sorted(head, &n);

    printf("%sToday's Tasks%s\n", C_BLUE(), S_RESET());
    printf("Due              ID  Description\n");
    printf("---------------- --- ------------------------------\n");
    size_t printed_today=0;
    for (size_t i=0;i<n;i++) {
        const Task *t = &arr[i]->task;
        if (t->due && is_today_local(t->due)) {
            print_task_row(t);
            printed_today++;
        }
    }
    if (!printed_today) printf(" (none)\n");
    printf("\n");

    printf("%sTomorrow's Tasks%s\n", C_BLACK(), S_RESET());
    printf("Due              ID  Description\n");
    printf("---------------- --- ------------------------------\n");
    size_t printed_tom=0;
    for (size_t i=0;i<n;i++) {
        const Task *t = &arr[i]->task;
        if (t->due && is_tomorrow_local(t->due)) {
            print_task_row(t);
            printed_tom++;
        }
    }
    if (!printed_tom) printf(" (none)\n");
    printf("\n");

    printf("%sAll Tasks%s (sorted by due; undated last)\n", C_BLUE(), S_RESET());
    printf("Due              ID  Description\n");
    printf("---------------- --- ------------------------------\n");
    size_t limit = env_limit("CLITASK_ALL_LIMIT", 20);
    size_t to_print = (n < limit) ? n : limit;
    for (size_t i = 0; i < to_print; i++)
	    print_task_row(&arr[i]->task);
    if (n > limit)
	    printf("... (%zu more)\n", n - limit);
    
    free(arr);
}

static void cmd_delete(int argc, char **argv){
    if (argc<1){
        printf("Usage: delete <id>\n");
        return;
    }
    int id=0;
    if(parseInt(argv[0],&id)!=0){
        printf("Invalid id.\n");
        return;
    }
    Task t;
    if(!list_remove_by_id(&head,id,&t)){
        printf("Task %d not found.\n", id);
        return;
    }
    list_push_head(&trash_head,t);
    printf("%sRemoved%s #%d.\n", C_RED(), S_RESET(), id);
    save_all_quiet();
}

static void cmd_removed(int argc, char **argv){
    (void)argc; (void)argv;
    if(!trash_head){
        printf("Removed is empty.\n");
        return;
    }
    printf("%sRemoved Tasks%s\n", C_RED(), S_RESET());
    printf("Due              ID  Description\n");
    printf("---------------- --- ------------------------------\n");
    for(Node *n=trash_head;n;n=n->next){
        char when[32];
        fmt_when(n->task.due, when, sizeof when);
        printf("%-16s %-3d %s\n", when, n->task.id, n->task.description);
    }
}

static void cmd_save(int argc, char **argv){
    (void)argc; (void)argv;
    save_all_verbose();
}

static volatile sig_atomic_t srv_running = 1;

static void handle_sigint(int sig){
    (void)sig;
    srv_running = 0;
}

static void http_send_header(int fd, const char *status, const char *ctype){
    dprintf(fd,
        "HTTP/1.0 %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, ctype);
}

static void write_all_tasks_text(int fd){
    size_t n=0;
    Node **arr=collect_sorted(head,&n);
    printf("Due              ID  Description\n");
    printf("---------------- --- ------------------------------\n");
    for (size_t i=0;i<n;i++){
        char when[32];
        fmt_when(arr[i]->task.due, when, sizeof when);
        dprintf(fd, "%-16s %-3d %s\n",
            when, arr[i]->task.id, arr[i]->task.description);
    }
    free(arr);
}

static void write_all_tasks_json(int fd){
    size_t n=0;
    Node **arr=collect_sorted(head,&n);
    dprintf(fd, "[\n");
    for (size_t i=0;i<n;i++){
        char when[32];
        fmt_when(arr[i]->task.due, when, sizeof when);
        dprintf(fd,
            " {\"id\":%d,\"due\":%lld,\"when\":\"%s\",\"description\":\"",
            arr[i]->task.id, (long long)arr[i]->task.due, when);
        const char *s = arr[i]->task.description;
        for (; *s; ++s){
            if (*s=='\"' || *s=='\\') dprintf(fd, "\\%c", *s);
            else if ((unsigned char)*s < 0x20) dprintf(fd, " ");
            else dprintf(fd, "%c", *s);
        }
        dprintf(fd, "\"}%s\n", (i+1<n)? ",": "");
    }
    dprintf(fd, "]\n");
    free(arr);
}

static void cmd_serve(int argc, char **argv){
    if (argc < 1){
        printf("Usage: serve <port>\n");
        return;
    }
    int port=0;
    if(parseInt(argv[0], &port)!=0 || port<=0 || port>65535){
        printf("Invalid port.\n");
        return;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0){ perror("socket"); return; }
    int one=1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&addr, sizeof addr) < 0){
        perror("bind"); close(s); return;
    }
    if (listen(s, 16) < 0){
        perror("listen"); close(s); return;
    }
    printf("%sServing%s on http://127.0.0.1:%d (Ctrl+C to stop)\n",
           C_BLUE(), S_RESET(), port);
    signal(SIGINT, handle_sigint);
    while (srv_running){
        int c = accept(s, NULL, NULL);
        if (c < 0){
            if (errno == EINTR) break;
            perror("accept"); continue;
        }
        char buf[2048];
        ssize_t n = read(c, buf, sizeof buf - 1);
        if (n < 0){ perror("read"); close(c); continue; }
        buf[n>0?n:0] = '\0';
        char method[8]={0}, path[256]={0};
        if (sscanf(buf, "%7s %255s", method, path) != 2) {
            http_send_header(c, "400 Bad Request", "text/plain");
            dprintf(c, "Bad Request\n");
            close(c);
            continue;
        }
        reload_all_from_disk();
        if (strcmp(path, "/json")==0){
            http_send_header(c, "200 OK", "application/json");
            write_all_tasks_json(c);
        } else {
            http_send_header(c, "200 OK", "text/plain");
            dprintf(c, "CLI Task Manager (HTTP view)\n\n");
            write_all_tasks_text(c);
            dprintf(c, "\nTip: GET /json for JSON.\n");
        }
        close(c);
    }
    close(s);
    printf("\nServer stopped.\n");
}

static bool due_within_minutes(time_t due, time_t now, int lead_min){
    if (!due) return false;
    double mins = difftime(due, now)/60.0;
    return (mins <= lead_min && mins >= 0.0);
}

#define SEEN_MAX 256
static int seen_ids[SEEN_MAX];
static size_t seen_len = 0;

static bool seen_has(int id){
    for (size_t i=0;i<seen_len;i++)
        if (seen_ids[i]==id) return true;
    return false;
}

static void seen_add(int id){
    if (seen_len < SEEN_MAX) seen_ids[seen_len++] = id;
}

static void notify_task(const Task *t, int argc, char **argv){
    if (argc <= 0){
        char when[32];
        fmt_when(t->due, when, sizeof when);
        printf("[REMINDER] #%d due %s : %s\n",
               t->id, when, t->description);
        fflush(stdout);
        return;
    }
    char cmd[1024];
    size_t off = 0;
    for (int i=0; i<argc; i++){
        int w = snprintf(cmd+off, sizeof cmd - off, "%s%s",
                         (i? " ":""), argv[i]);
        if (w < 0 || (size_t)w >= sizeof cmd - off) break;
        off += (size_t)w;
    }
    char when[32];
    fmt_when(t->due, when, sizeof when);
    int w = snprintf(cmd+off, sizeof cmd - off,
                     " \"%s (due %s)\"", t->description, when);
    (void)w;
    system(cmd);
}

static void detach_from_terminal(void){
    if (setsid() < 0) { }
    signal(SIGHUP, SIG_IGN);
}

static void cmd_watch(int argc, char **argv){
    int interval = (argc>=1)? atoi(argv[0]) : 60;
    if (interval <= 0) interval = 60;
    int lead_min = (argc>=2)? atoi(argv[1]) : 10;
    if (lead_min <= 0) lead_min = 10;
    int notify_argc = (argc>=3)? (argc-2) : 0;
    char **notify_argv = (argc>=3)? (&argv[2]) : NULL;
    pid_t pid = fork();
    if (pid < 0){ perror("fork"); return; }
    if (pid > 0){
        printf("Watcher started (pid=%d). Scanning every %ds; lead=%dmin.\n",
               (int)pid, interval, lead_min);
        return;
    }
    detach_from_terminal();
    while (1){
        reload_all_from_disk();
        time_t now = time(NULL);
        for (Node *p=head; p; p=p->next){
            if (!p->task.due) continue;
            if (due_within_minutes(p->task.due, now, lead_min) &&
                !seen_has(p->task.id)){
                notify_task(&p->task, notify_argc, notify_argv);
                seen_add(p->task.id);
            }
        }
        sleep((unsigned)interval);
    }
}

typedef void (*handler_t)(int,char**);
typedef struct { const char *name; handler_t fn; } Command;

static void cmd_delete_alias(int argc, char **argv){
    cmd_delete(argc, argv);
}

static Command CMDS[] = {
    {"add", cmd_add},
    {"list", cmd_list},
    {"delete", cmd_delete},
    {"remove", cmd_delete_alias},
    {"removed", cmd_removed},
    {"save", cmd_save},
    {"help", cmd_help},
    {"serve", cmd_serve},
    {"watch", cmd_watch},
    {NULL, NULL}
};

static void load_all(void){
    load_file(active_file,&head,&nextId);
    load_file(removed_file,&trash_head,&nextId);
}

static void at_exit_cleanup(void){
    save_all_quiet();
    list_free(&head);
    list_free(&trash_head);
}

int main(int argc, char **argv){
    atexit(at_exit_cleanup);
    init_paths();
    load_all();
    if (argc<2){
        cmd_help(0,NULL);
        return 2;
    }
    const char *cmd=argv[1];
    for (int i=0; CMDS[i].name; ++i){
        if (strcmp(CMDS[i].name, cmd)==0) {
            CMDS[i].fn(argc-2, argv+2);
            return 0;
        }
    }
    printf("Unknown command: %s\nTry: %s help\n", cmd, argv[0]);
    return 2;
}

