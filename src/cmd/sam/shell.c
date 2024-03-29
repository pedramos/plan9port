#include "sam.h"
#include "parse.h"

extern	jmp_buf	mainloop;

char	errfile[64];
String	plan9cmd;	/* null terminated */
Buffer	plan9buf;
void	checkerrs(void);
Buffer	cmdbuf;
int	cmdbufpos;

static void
updateenv(File *f)
{
	static int fd = -1;
	int n;
	char buf[64], *p, *e;
	
	if(f == nil){
		putenv("%", "");
		putenv("%dot", "");
		return;
	}

	p = Strtoc(&f->name);
	putenv("%", p);
	free(p);
	
	p = buf;
	e = buf+sizeof(buf);
	p = seprint(p, e, "%lud", 1+nlcount(f, 0, f->dot.r.p1));
	p = seprint(p+1, e, "%lud", f->dot.r.p1);
	p = seprint(p+1, e, "%lud", f->dot.r.p2);
	n = p - buf;
	if(fd == -1)
	if(putenv("%dot", buf) < 0)
		fprint(2, "updateenv write: %r\n");
}

int
plan9(File *f, int type, String *s, int nest)
{
	long l;
	int m;
	int pid, fd;
	int retcode;
	char *retmsg;
	int pipe1[2], pipe2[2];

	if(s->s[0]==0 && plan9cmd.s[0]==0)
		error(Enocmd);
	else if(s->s[0])
		Strduplstr(&plan9cmd, s);
	if(downloaded){
		samerr(errfile);
		remove(errfile);
	}
	if(type!='!' && pipe(pipe1)==-1)
		error(Epipe);
	if(type=='|' || type=='_')
		snarf(f, addr.r.p1, addr.r.p2, &plan9buf, 1);
	if((pid=fork()) == 0){
		if(downloaded){	/* also put nasty fd's into errfile */
			fd = create(errfile, 1, 0666L);
			if(fd < 0)
				fd = create("/dev/null", 1, 0666L);
			dup(fd, 2);
			close(fd);
			/* 2 now points at err file */
			if(type == '>')
				dup(2, 1);
			else if(type=='!'){
				dup(2, 1);
				fd = open("/dev/null", 0);
				dup(fd, 0);
				close(fd);
			}
		}
		if(type != '!') {
			if(type == '>')
				dup(pipe1[0], 0);
			else
				dup(pipe1[1], 1);
			close(pipe1[0]);
			close(pipe1[1]);
		}
		if(type == '|' || type == '_'){
			if(pipe(pipe2) == -1)
				exits("pipe");
			if((pid = fork())==0){
				/*
				 * It's ok if we get SIGPIPE here
				 */
				close(pipe2[0]);
				io = pipe2[1];
				if(retcode=!setjmp(mainloop)){	/* assignment = */
					char *c;
					for(l = 0; l<plan9buf.nc; l+=m){
						m = plan9buf.nc-l;
						if(m>BLOCKSIZE-1)
							m = BLOCKSIZE-1;
						bufread(&plan9buf, l, genbuf, m);
						genbuf[m] = 0;
						c = Strtoc(tmprstr(genbuf, m+1));
						Write(pipe2[1], c, strlen(c));
						free(c);
					}
				}
				exits(retcode? "error" : 0);
			}
			if(pid==-1){
				fprint(2, "Can't fork?!\n");
				exits("fork");
			}
			dup(pipe2[0], 0);
			close(pipe2[0]);
			close(pipe2[1]);
		}
		if(type=='<' || type=='^'){
			close(0);	/* so it won't read from terminal */
			open("/dev/null", 0);
		}
		updateenv(f);
		execl(SHPATH, SH, "-c", Strtoc(&plan9cmd), nil);
		exits("exec");
	}
	if(pid == -1)
		error(Efork);
	if(type=='<' || type=='|'){
		int nulls;
		if(downloaded && addr.r.p1 != addr.r.p2)
			outTl(Hsnarflen, addr.r.p2-addr.r.p1);
		snarf(f, addr.r.p1, addr.r.p2, &snarfbuf, 0);
		logdelete(f, addr.r.p1, addr.r.p2);
		close(pipe1[1]);
		io = pipe1[0];
		f->tdot.p1 = -1;
		f->ndot.r.p2 = addr.r.p2+readio(f, &nulls, 0, FALSE);
		f->ndot.r.p1 = addr.r.p2;
		closeio((Posn)-1);
	}else if(type=='>'){
		close(pipe1[0]);
		io = pipe1[1];
		bpipeok = 1;
		writeio(f);
		bpipeok = 0;
		closeio((Posn)-1);
	}else if(type == '^' || type == '_'){
		int nulls;
		close(pipe1[1]);
		bufload(&cmdbuf, cmdbufpos, pipe1[0], &nulls);
		close(pipe1[0]);
	}
	retmsg = waitfor(pid);
	if(type=='|' || type=='<' || type=='_' || type=='^')
		if(retmsg[0]!=0)
			warn_s(Wbadstatus, retmsg);
	if(downloaded)
		checkerrs();
	if(!nest)
		dprint("!\n");
	return retmsg[0] ? -1 : 0;
}

void
checkerrs(void)
{
	char buf[BLOCKSIZE-10];
	int f, n, nl;
	char *p;
	long l;

	if(statfile(errfile, 0, 0, 0, &l, 0) > 0 && l != 0){
		if((f=open(errfile, 0)) != -1){
			if((n=read(f, buf, sizeof buf-1)) > 0){
				for(nl=0,p=buf; nl<25 && p<&buf[n]; p++)
					if(*p=='\n')
						nl++;
				*p = 0;
				dprint("%s", buf);
				if(p-buf < l-1)
					dprint("(sam: more in %s)\n", errfile);
			}
			close(f);
		}
	}else
		remove(errfile);
}
