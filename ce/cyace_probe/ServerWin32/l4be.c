/*
 * $Id: l4be.c,v 1.1 2004/08/28 21:29:33 be300 Exp $
 *
 * l4be.c - server part of the network loader 
 *
 * Copyright (C) 2004 Mouse & Filip Onkelinx <www.Linux4.BE>
 *
 * $Author: be300 $
 * $Date: 2004/08/28 21:29:33 $
 * $Revision: 1.1 $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <winsock2.h>
#include <linux/elf.h>
#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DEBUG 1

FILE *log = NULL;

enum
{
    INFO = 1, LOAD = 2
};

#if DEBUG
#define debug_printf printf
#define WOUT(a,b,c) {}
#else
//#define debug_printf(fmt,a...) fprintf(log,fmt,##a)
#define debug_printf(fmt,a...) {}
#define WOUT write
#endif

static int vmem_sub(int fd, caddr_t addr, long nbytes, long *byte_count,  SOCKET client)
{
	int n = 4096;
	caddr_t end_addr;
	char buf[4096];

    fprintf(log,"\taddr:%x nbytes:%x\r\n",(unsigned long)addr,nbytes);
    fflush(log);
	for (end_addr = addr + nbytes;
	     addr < end_addr;
	     addr += n) {
		if (end_addr < (addr + n)) {
			n = end_addr - addr;
		}
		if (read(fd, &buf, n) != n) {
            fprintf(log,"read segment error %x bytes @%x\n",n,(unsigned long)addr);
            fflush(log);
			return -1;
		}
		send(client, (char *)&buf, n,0);
		*byte_count += n;
	}
    fflush(log);
	return 0;
}

static int scanfile(int fd, caddr_t *start, caddr_t *end, caddr_t *entry, int load, SOCKET client)
{
	Elf32_Ehdr elfx, *elf = &elfx;
	int i, first;
	long byte_count;
	int progress;
	Elf32_Phdr *phtbl = NULL;
	caddr_t min_addr, max_addr;

	if (read(fd, (void*)elf, sizeof(Elf32_Ehdr)) != sizeof(Elf32_Ehdr)) {
		debug_printf("read header error\n");
		goto error_cleanup;
	}

	if (load)
		send(client, (char *)elf, sizeof(Elf32_Ehdr),0);

	if ((phtbl = (Elf32_Phdr *)malloc(sizeof(*phtbl) * elf->e_phnum)) == NULL) {
		debug_printf("alloc() error\n");
		goto error_cleanup;
	}

	if (lseek(fd, elf->e_phoff, SEEK_SET) == -1)  {
		debug_printf("seek for program header table error\n");
		goto error_cleanup;
	}

	if (read(fd, (void *)phtbl, sizeof(Elf32_Phdr) * elf->e_phnum)
				!= (int)(sizeof(Elf32_Phdr) * elf->e_phnum)) {
		debug_printf("read program header table error\n");
		goto error_cleanup;
	}

    fprintf(log,"\tsend phtbl (%d)\r\n",sizeof(Elf32_Phdr) * elf->e_phnum);
    fflush(log);
	if (load)
		send(client, (char *)phtbl, sizeof(Elf32_Phdr) * elf->e_phnum,0);

	/*
	 *  scan program header table
	 */
	first = 1;
	byte_count = 0;
	progress = 0;
	for (i = 0; i < elf->e_phnum; i++) {
		if (phtbl[i].p_type != PT_LOAD) {
			continue;
		}

		if (first || max_addr < (caddr_t)(phtbl[i].p_vaddr + phtbl[i].p_memsz)) {
			max_addr = (caddr_t)(phtbl[i].p_vaddr + phtbl[i].p_memsz);
		}
		if (first || (caddr_t)phtbl[i].p_vaddr < min_addr) {
			min_addr = (caddr_t)phtbl[i].p_vaddr;
		}

		if (load) {
			if (lseek(fd, phtbl[i].p_offset, SEEK_SET) == -1)  {
				debug_printf("seek for segment error\n");
				goto error_cleanup;
			}

			if (vmem_sub(fd,
				     (caddr_t)phtbl[i].p_vaddr,
				     phtbl[i].p_filesz,
				     &byte_count,client) != 0) {
				goto error_cleanup;
			}
		} else {
			byte_count += phtbl[i].p_memsz;
		}

		first = 0;
	}

	if (first) {
		debug_printf("can't find loadable segment\n");
		goto error_cleanup;
	}

	//debug_printf("entry=%x  addr=%x-%x\n",
	//	     elf->e_entry, min_addr, max_addr);

	if (phtbl) free(phtbl);

	if (start) *start = min_addr;
	if (end) *end = max_addr;
	if (entry) *entry = (caddr_t)elf->e_entry;
	return (0);

error_cleanup:
	if (phtbl) free(phtbl);
	return -1;
}

int handleConnection(char *home, SOCKET client, struct sockaddr_in *clntAddr)
{
    short int len;
    int msgsize;
    char fname[512];
    char path[512];
    char action = 0;
   FILE *fp = NULL;

    fprintf(log,"Connection from IP %s\r\n",inet_ntoa(clntAddr->sin_addr));
    fflush(log);       
    caddr_t start = NULL, end = NULL, entry = NULL;
    msgsize=recv(client, (char *)&len, 4,0);
    if (msgsize<0){
        fprintf(log, "***error reading command\n");
        fflush(log);       
        return(-1);
    }
    if (len>510){
        fprintf(log, "***fname overflow\n");
        fflush(log);       
        return(-1);        
    }    
    recv(client, fname, len,0);
    fname[len] = '\0';
    recv(client, &action, 1, 0);

    fprintf(log, "action=%s fname=%s\n", action == INFO ? "INFO" : "LOAD", fname);
    fflush(log);
    sprintf(path, "%s/%s", home, fname);
    fp = fopen(path, "rb");
    if (!fp) {
    	fprintf(log,"Unable to open file (%s): %s\n", path, strerror(errno));    
   		fflush(log);
    	return 0;
    }
    switch (action) {
	case INFO:
	    scanfile(fileno(fp), &start, &end, NULL, 0,client);
	    if (start && end) {
             fprintf(log,"\tstart:%x end%x\r\n",(unsigned long)start,(unsigned long)end);
             send(client, (char *)&start, sizeof(start),0);
             send(client, (char *)&end, sizeof(end),0);
	    }
	    break;
	case LOAD:
	    scanfile(fileno(fp), NULL, NULL, &entry, 1,client);
	    if (entry){
                fprintf(log,"\tentry:%x\r\n",(unsigned long)entry);
			    send(client, (char *)&entry, sizeof(entry),0);
		}		
	    break;
    }
    shutdown(client,1);
    closesocket(client);
    fclose(fp);
    fprintf(log,"Closing client %s\n", inet_ntoa(clntAddr->sin_addr));
    fflush(log);       
    return 0;
}



int main(int argc, char **argv)
{
SOCKET server;
WSADATA wsaData;
sockaddr_in local;
    SOCKET client;
    sockaddr_in from;
    int fromlen=sizeof(from);

 

    if (argc != 2)
	return -1;

    log = fopen("l4be.log", "a");
    fprintf(log, "l4be server started ( $Id: l4be.c,v 1.1 2004/08/28 21:29:33 be300 Exp $ )\n");
    fflush(log);       

    int wsaret=WSAStartup(0x101,&wsaData);
    if(wsaret!=0)
    {
        fprintf(log, "failed to initialise winsock\n");
        fflush(log);       
        fclose(log);
        return 0;
    }
    local.sin_family=AF_INET; //Address family
    local.sin_addr.s_addr=INADDR_ANY; //Wild card IP address
    local.sin_port=htons((u_short)13131); //port to use
    server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(server==INVALID_SOCKET)
    {
        return 0;
    }
    if(bind(server,(sockaddr*)&local,sizeof(local))!=0)
    {
        return 0;
    }
    if(listen(server,10)!=0) // Max 10 pending connections
    {
        return 0;
    }
    
    while(true)//we are looping endlessly
    {
        client=accept(server, (struct sockaddr*)&from,&fromlen);
        handleConnection( argv[1],client,&from);
    }
    closesocket(server);
    fclose(log);
    WSACleanup();

}

