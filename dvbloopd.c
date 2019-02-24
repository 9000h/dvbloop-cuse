/*
 * CUSE based DVB loop sample code
 *
 * Copyright (c) 2016 Andreas Steinmetz (ast@domdv.de)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 *
 */

#include <sys/ioctl.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>

#include "dvbcuse.h"

static int sys_open(void *user,const char *pathname,int flags)
{
	return open(pathname,flags);
}

static ssize_t sys_read(void *user,int fd,void *buf,size_t count)
{
	return read(fd,buf,count);
}

static ssize_t sys_write(void *user,int fd,const void *buf,size_t count)
{
	return write(fd,buf,count);
}

static void sys_close(void *user,int fd)
{
	close(fd);
}

static int sys_ioctl(void *user,int fd, unsigned long request,void *arg)
{
	return ioctl(fd,request,arg);
}

static int sys_poll(void *user,struct pollfd *fd)
{
	return poll(fd,1,0);
}

static void usage(void)
{
	fprintf(stderr,"Usage: dvbloopd [params]\n"
	"-s source       source dvb adapter number\n"
	"-a adapter      lopp dvb adapter number (target)\n"
	"-m major        major device number\n"
	"-M minor-base   minor device base number (multiple of 8)\n"
	"-o owner        device uid\n"
	"-g group        device gid\n"
	"-p perms        device permission (octal)\n"
	"-F              disable frontend device\n"
	"-D              disable demux device\n"
	"-V              disable dvr device\n"
	"-C              disable ca device\n"
	"-N              disable net device\n");

	exit(1);
}

int main(int argc,char *argv[])
{
	DVBCUSE_DEVICE dev;
	void *ctx;
	sigset_t set;
	int source=4;
	int c;

	memset(&dev,0,sizeof(dev));

	dev.major=256;

	dev.owner=0;
	dev.group=0;
	dev.perms=0666;

	dev.fe_enabled=1;
	dev.dmx_enabled=1;
	dev.dvr_enabled=1;
	dev.ca_enabled=1;
	dev.net_enabled=1;

	while((c=getopt(argc,argv,"a:m:M:o:g:p:FDVCNs:"))!=-1)switch(c)
	{
	case 'a':
		dev.adapter=atoi(optarg);
		break;

	case 'm':
		dev.major=atoi(optarg);
		break;

	case 'M':
		dev.minbase=atoi(optarg);
		break;

	case 'o':
		dev.owner=atoi(optarg);
		break;

	case 'g':
		dev.group=atoi(optarg);
		break;

	case 'p':
		dev.perms=(int)strtol(optarg,NULL,8);
		break;

	case 'F':
		dev.fe_enabled=0;
		break;

	case 'D':
		dev.dmx_enabled=0;
		break;

	case 'V':
		dev.dvr_enabled=0;
		break;

	case 'C':
		dev.ca_enabled=0;
		break;

	case 'N':
		dev.net_enabled=0;
		break;

	case 's':
		source=atoi(optarg);
		break;

	default:usage();
	}

	if(dev.adapter==source||!dev.major)usage();

	sprintf(dev.fe_pathname,"/dev/dvb/adapter%d/frontend0",source);
	sprintf(dev.dmx_pathname,"/dev/dvb/adapter%d/demux0",source);
	sprintf(dev.dvr_pathname,"/dev/dvb/adapter%d/dvr0",source);
	sprintf(dev.ca_pathname,"/dev/dvb/adapter%d/ca0",source);
	sprintf(dev.net_pathname,"/dev/dvb/adapter%d/net0",source);

	dev.fe_open=sys_open;
	dev.fe_close=sys_close;
	dev.fe_ioctl=sys_ioctl;
	dev.fe_poll=sys_poll;

	dev.dmx_open=sys_open;
	dev.dmx_read=sys_read;
	dev.dmx_close=sys_close;
	dev.dmx_ioctl=sys_ioctl;
	dev.dmx_poll=sys_poll;

	dev.dvr_open=sys_open;
	dev.dvr_read=sys_read;
	dev.dvr_write=sys_write;
	dev.dvr_close=sys_close;
	dev.dvr_ioctl=sys_ioctl;
	dev.dvr_poll=sys_poll;

	dev.ca_open=sys_open;
	dev.ca_read=sys_read;
	dev.ca_write=sys_write;
	dev.ca_close=sys_close;
	dev.ca_ioctl=sys_ioctl;
	dev.ca_poll=sys_poll;

	dev.net_open=sys_open;
	dev.net_close=sys_close;
	dev.net_ioctl=sys_ioctl;

	if((ctx=dvbcuse_create(&dev)))
	{
		sigemptyset(&set);
		sigsuspend(&set);

		dvbcuse_destroy(ctx);
	}
	else return 1;

	return 0;
}
