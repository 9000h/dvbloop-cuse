/*
 * CUSE based DVB loop driver
 *
 * Copyright (c) 2016 Andreas Steinmetz (ast@domdv.de)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 *
 */

#ifndef DVB_CUSE_H
#define DVB_CUSE_H

#ifndef CA_SET_PID /* removed in kernel 4.14 */
typedef struct ca_pid {
        unsigned int pid;
        int index;          /* -1 == disable */
} ca_pid_t;
#define CA_SET_PID _IOW('o', 135, struct ca_pid)
#endif

typedef struct
{
	int adapter;
	int major;
	int minbase;

	int owner;
	int group;
	int perms;

	int fe_enabled:1;
	int dmx_enabled:1;
	int dvr_enabled:1;
	int ca_enabled:1;
	int net_enabled:1;

	char fe_pathname[PATH_MAX];
	char dmx_pathname[PATH_MAX];
	char dvr_pathname[PATH_MAX];
	char ca_pathname[PATH_MAX];
	char net_pathname[PATH_MAX];

	int (*fe_open)(void *user,const char *pathname,int flags);
	void (*fe_close)(void *user,int fd);
	int (*fe_ioctl)(void *user,int fd,unsigned long request,void *arg);
	int (*fe_poll)(void *user,struct pollfd *fd);

	int (*dmx_open)(void *user,const char *pathname,int flags);
	ssize_t (*dmx_read)(void *user,int fd,void *buf,size_t count);
	void (*dmx_close)(void *user,int fd);
	int (*dmx_ioctl)(void *user,int fd,unsigned long request,void *arg);
	int (*dmx_poll)(void *user,struct pollfd *fd);

	int (*dvr_open)(void *user,const char *pathname,int flags);
	ssize_t (*dvr_read)(void *user,int fd,void *buf,size_t count);
	ssize_t (*dvr_write)(void *user,int fd,const void *buf,size_t count);
	void (*dvr_close)(void *user,int fd);
	int (*dvr_ioctl)(void *user,int fd, unsigned long request,void *arg);
	int (*dvr_poll)(void *user,struct pollfd *fd);

	int (*ca_open)(void *user,const char *pathname,int flags);
	ssize_t (*ca_read)(void *user,int fd,void *buf,size_t count);
	ssize_t (*ca_write)(void *user,int fd,const void *buf,size_t count);
	void (*ca_close)(void *user,int fd);
	int (*ca_ioctl)(void *user,int fd,unsigned long request,void *arg);
	int (*ca_poll)(void *user,struct pollfd *fd);

	int (*net_open)(void *user,const char *pathname,int flags);
	void (*net_close)(void *user,int fd);
	int (*net_ioctl)(void *user,int fd,unsigned long request,void *arg);

	void *user;
} DVBCUSE_DEVICE;


extern void *dvbcuse_create(DVBCUSE_DEVICE *config);
extern void dvbcuse_destroy(void *ctx);

#endif
