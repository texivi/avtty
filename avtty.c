#include<unistd.h>
#include<fcntl.h>
#include<termios.h>
char b[1<<20],c,s[2];int n,p,f;struct termios t,q;
void d(){write(1,"\e[H\e[J",6);write(1,b,p);if(p<n)write(1,"\e[7m",4),write(1,b+p,1),write(1,"\e[m",3),write(1,b+p+1,n-p-1);}
int main(int a,char**v){if(a<2||(f=open(v[1],2))<0)return 1;n=read(f,b,1<<20);tcgetattr(0,&t);q=t;q.c_lflag&=~10;tcsetattr(0,0,&q);d();while(read(0,&c,1)&&c^17){if(c==19)ftruncate(f,pwrite(f,b,n,0));else if(c==127&&p)for(--p,--n;p<n;p++)b[p]=b[p+1];else if(c==27)read(0,s,2),p+=s[1]&1?p<n:!p-1;else if(c>31|c==10){for(int i=n++;i>p;i--)b[i]=b[i-1];b[p++]=c;}d();}tcsetattr(0,0,&t);}
