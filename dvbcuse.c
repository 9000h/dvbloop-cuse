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

#define FUSE_USE_VERSION 29

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/ca.h>
#include <linux/dvb/net.h>

#include <cuse_lowlevel.h>
#include <fuse_lowlevel.h>
#include <fuse_opt.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>

#include "dvbcuse.h"

typedef struct
{
	struct _stream *s;
	pthread_mutex_t mtx;
	pthread_t th[5];
	DVBCUSE_DEVICE conf;
} DATA;

typedef struct _stream
{
	struct _stream *next;
	int flags;
	DATA *dev;
	int fd;
} STREAM;

static const struct fuse_opt dvbtvd_opts[]=
{
	FUSE_OPT_END
};

static int dvbtvd_args(void *data,const char *arg,int key,
	struct fuse_args *outargs)
{
	return 1;
}

static void net_post(void *userdata)
{
	DATA *dev=(DATA *)userdata;
	char devpath[PATH_MAX];

	sprintf(devpath,"/dev/dvb/adapter%d/net0",dev->conf.adapter);
	chown(devpath,dev->conf.owner,dev->conf.group);
	chmod(devpath,dev->conf.perms);
}

static void net_open(fuse_req_t req,struct fuse_file_info *fi)
{
	DATA *dev=fuse_req_userdata(req);
	STREAM *s;

	if(!dev)
	{
		fuse_reply_err(req,EINVAL);
		return;
	}

	if(!dev->conf.net_open)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(!(s=malloc(sizeof(STREAM))))
	{
		fuse_reply_err(req,EMFILE);
		return;
	}

	memset(s,0,sizeof(STREAM));
	s->flags=fi->flags;
	s->dev=dev;

	if((s->fd=dev->conf.net_open(dev->conf.user,dev->conf.net_pathname,
		fi->flags))==-1)
	{
		fuse_reply_err(req,errno);
		free(s);
		return;
	}

	pthread_mutex_lock(&dev->mtx);

	s->next=dev->s;
	dev->s=s;

	pthread_mutex_unlock(&dev->mtx);

	fi->direct_io=1;
	fi->keep_cache=0;
	fi->nonseekable=1;
	fi->fh=(uint64_t)s;

	fuse_reply_open(req,fi);
}

static void net_read(fuse_req_t req,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;

	if((s->flags&O_ACCMODE)==O_WRONLY)fuse_reply_err(req,EPERM);
	else fuse_reply_err(req,EOPNOTSUPP);
}

static void net_write(fuse_req_t req,const char *buf,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;

	if((s->flags&O_ACCMODE)==O_RDONLY)fuse_reply_err(req,EPERM);
	else fuse_reply_err(req,EOPNOTSUPP);
}

static void net_flush(fuse_req_t req,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void net_release(fuse_req_t req,struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	STREAM **e;

	if(!dev->conf.net_close)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	dev->conf.net_close(dev->conf.user,s->fd);

	pthread_mutex_lock(&dev->mtx);

	for(e=&dev->s;*e;e=&(*e)->next)if(*e==s)
	{
		*e=s->next;
		free(s);
		break;
	}

	pthread_mutex_unlock(&dev->mtx);

	fuse_reply_err(req,0);
}

static void net_fsync(fuse_req_t req,int datasync,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void net_ioctl(fuse_req_t req,int cmd,void *arg,
	struct fuse_file_info *fi,unsigned flags,const void *in_buf,
	size_t in_bufsz,size_t out_bufsz)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct iovec iov;
	union
	{
		struct dvb_net_if *in;
		struct dvb_net_if out;
	}u;

	if(!dev->conf.net_ioctl)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(flags&FUSE_IOCTL_COMPAT)fuse_reply_err(req,ENOSYS);
	else switch(cmd)
	{
	case NET_REMOVE_IF:
		if(dev->conf.net_ioctl(dev->conf.user,s->fd,cmd,NULL)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	case NET_ADD_IF:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_net_if);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.in=(struct dvb_net_if *)in_buf;
			if(dev->conf.net_ioctl(dev->conf.user,s->fd,cmd,u.in)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case NET_GET_IF:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_net_if);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.net_ioctl(dev->conf.user,s->fd,cmd,&u.out)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.out,
				sizeof(struct dvb_net_if));
		}
		break;

	default:
		fuse_reply_err(req,EINVAL);
		break;
	}
}

static void net_poll(fuse_req_t req,struct fuse_file_info *fi,
	struct fuse_pollhandle *ph)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static const struct cuse_lowlevel_ops net_ops=
{
	.init_done=net_post,
	.open=net_open,
	.read=net_read,
	.write=net_write,
	.flush=net_flush,
	.release=net_release,
	.fsync=net_fsync,
	.ioctl=net_ioctl,
	.poll=net_poll,
};

static void *networker(void *data)
{
	struct cuse_info ci;
	DATA *dev=(DATA *)data;
	char devpath[PATH_MAX+9];
	const char *devarg[1]={devpath};
	int dummy_argc=1;
	char *dummy_argv[66]={""};
	struct fuse_args args=FUSE_ARGS_INIT(dummy_argc,dummy_argv);
	sigset_t set;

	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK,&set,NULL);

	if(fuse_opt_parse(&args,NULL,dvbtvd_opts,dvbtvd_args))goto out;
	if(fuse_opt_add_arg(&args, "-f"))goto out;

	sprintf(devpath,"DEVNAME=dvb/adapter%d/net0",dev->conf.adapter);
	memset(&ci,0,sizeof(ci));
	ci.dev_major=dev->conf.major;
	ci.dev_minor=dev->conf.minbase+4;
	ci.dev_info_argc=1;
	ci.dev_info_argv=devarg;
	ci.flags=CUSE_UNRESTRICTED_IOCTL;

	cuse_lowlevel_main(args.argc,args.argv,&ci,&net_ops,data);

out:	fuse_opt_free_args(&args);

	pthread_exit(NULL);
}

static void ca_post(void *userdata)
{
	DATA *dev=(DATA *)userdata;
	char devpath[PATH_MAX];

	sprintf(devpath,"/dev/dvb/adapter%d/ca0",dev->conf.adapter);
	chown(devpath,dev->conf.owner,dev->conf.group);
	chmod(devpath,dev->conf.perms);
}

static void ca_open(fuse_req_t req,struct fuse_file_info *fi)
{
	DATA *dev=fuse_req_userdata(req);
	STREAM *s;

	if(!dev)
	{
		fuse_reply_err(req,EINVAL);
		return;
	}

	if(!dev->conf.ca_open)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(!(s=malloc(sizeof(STREAM))))
	{
		fuse_reply_err(req,EMFILE);
		return;
	}

	memset(s,0,sizeof(STREAM));
	s->flags=fi->flags;
	s->dev=dev;

	if((s->fd=dev->conf.ca_open(dev->conf.user,dev->conf.ca_pathname,
		fi->flags))==-1)
	{
		fuse_reply_err(req,errno);
		free(s);
		return;
	}

	pthread_mutex_lock(&dev->mtx);

	s->next=dev->s;
	dev->s=s;

	pthread_mutex_unlock(&dev->mtx);

	fi->direct_io=1;
	fi->keep_cache=0;
	fi->nonseekable=1;
	fi->fh=(uint64_t)s;

	fuse_reply_open(req,fi);
}

static void ca_read(fuse_req_t req,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	ssize_t len;
	char bfr[131072];

	if((s->flags&O_ACCMODE)==O_WRONLY)
	{
		fuse_reply_err(req,EPERM);
		return;
	}

	if(!dev->conf.ca_read)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if((len=dev->conf.ca_read(dev->conf.user,s->fd,bfr,
		size>sizeof(bfr)?sizeof(bfr):size))==-1)
	{
		fuse_reply_err(req,errno);
		return;
	}

	fuse_reply_buf(req,bfr,len);
}

static void ca_write(fuse_req_t req,const char *buf,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	ssize_t len;

	if((s->flags&O_ACCMODE)==O_RDONLY)
	{
		fuse_reply_err(req,EPERM);
		return;
	}

	if(!dev->conf.ca_write)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if((len=dev->conf.ca_write(dev->conf.user,s->fd,buf,size))==-1)
	{
		fuse_reply_err(req,errno);
		return;
	}

	fuse_reply_write(req,len);
}

static void ca_flush(fuse_req_t req,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void ca_release(fuse_req_t req,struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	STREAM **e;

	if(!dev->conf.ca_close)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	dev->conf.ca_close(dev->conf.user,s->fd);

	pthread_mutex_lock(&dev->mtx);

	for(e=&dev->s;*e;e=&(*e)->next)if(*e==s)
	{
		*e=s->next;
		free(s);
		break;
	}

	pthread_mutex_unlock(&dev->mtx);

	fuse_reply_err(req,0);
}

static void ca_fsync(fuse_req_t req,int datasync,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void ca_ioctl(fuse_req_t req,int cmd,void *arg,
	struct fuse_file_info *fi,unsigned flags,const void *in_buf,
	size_t in_bufsz,size_t out_bufsz)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct iovec iov;
	union
	{
		ca_caps_t caps;
		ca_slot_info_t sinfo;
		ca_descr_info_t dinfo;
		ca_msg_t mout;
		ca_msg_t *min;
		ca_descr_t *desc;
		ca_pid_t *pid;
	}u;

	if(!dev->conf.ca_ioctl)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(flags&FUSE_IOCTL_COMPAT)fuse_reply_err(req,ENOSYS);
	else switch(cmd)
	{
	case CA_RESET:
		if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,NULL)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	case CA_GET_CAP:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_caps_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,&u.caps)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.caps,
				sizeof(ca_caps_t));
		}
		break;

	case CA_GET_SLOT_INFO:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_slot_info_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,&u.sinfo)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.sinfo,
				sizeof(ca_slot_info_t));
		}
		break;

	case CA_GET_DESCR_INFO:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_descr_info_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,&u.dinfo)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.dinfo,
				sizeof(ca_descr_info_t));
		}
		break;

	case CA_GET_MSG:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_msg_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,&u.mout)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.mout,
				sizeof(ca_msg_t));
		}
		break;

	case CA_SEND_MSG:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_msg_t);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.min=(ca_msg_t *)in_buf;
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,u.min)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case CA_SET_DESCR:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_descr_t);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.desc=(ca_descr_t *)in_buf;
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,u.desc)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case CA_SET_PID:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(ca_pid_t);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.pid=(ca_pid_t *)in_buf;
			if(dev->conf.ca_ioctl(dev->conf.user,s->fd,cmd,u.pid)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	default:
		fuse_reply_err(req,EINVAL);
		break;
	}
}

static void ca_poll(fuse_req_t req,struct fuse_file_info *fi,
	struct fuse_pollhandle *ph)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct pollfd p;

	if(!dev->conf.ca_poll)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	p.fd=s->fd;
	p.events=POLLIN;
	p.revents=0;

	dev->conf.ca_poll(dev->conf.user,&p);

	fuse_reply_poll(req,p.revents);
	if(p.revents&&ph)fuse_lowlevel_notify_poll(ph);
}

static const struct cuse_lowlevel_ops ca_ops=
{
	.init_done=ca_post,
	.open=ca_open,
	.read=ca_read,
	.write=ca_write,
	.flush=ca_flush,
	.release=ca_release,
	.fsync=ca_fsync,
	.ioctl=ca_ioctl,
	.poll=ca_poll,
};

static void *caworker(void *data)
{
	struct cuse_info ci;
	DATA *dev=(DATA *)data;
	char devpath[PATH_MAX+9];
	const char *devarg[1]={devpath};
	int dummy_argc=1;
	char *dummy_argv[66]={""};
	struct fuse_args args=FUSE_ARGS_INIT(dummy_argc,dummy_argv);
	sigset_t set;

	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK,&set,NULL);

	if(fuse_opt_parse(&args,NULL,dvbtvd_opts,dvbtvd_args))goto out;
	if(fuse_opt_add_arg(&args, "-f"))goto out;

	sprintf(devpath,"DEVNAME=dvb/adapter%d/ca0",dev->conf.adapter);
	memset(&ci,0,sizeof(ci));
	ci.dev_major=dev->conf.major;
	ci.dev_minor=dev->conf.minbase+3;
	ci.dev_info_argc=1;
	ci.dev_info_argv=devarg;
	ci.flags=CUSE_UNRESTRICTED_IOCTL;

	cuse_lowlevel_main(args.argc,args.argv,&ci,&ca_ops,data);

out:	fuse_opt_free_args(&args);

	pthread_exit(NULL);
}

static void dvr_post(void *userdata)
{
	DATA *dev=(DATA *)userdata;
	char devpath[PATH_MAX];

	sprintf(devpath,"/dev/dvb/adapter%d/dvr0",dev->conf.adapter);
	chown(devpath,dev->conf.owner,dev->conf.group);
	chmod(devpath,dev->conf.perms);
}

static void dvr_open(fuse_req_t req,struct fuse_file_info *fi)
{
	DATA *dev=fuse_req_userdata(req);
	STREAM *s;

	if(!dev)
	{
		fuse_reply_err(req,EINVAL);
		return;
	}

	if(!dev->conf.dvr_open)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(!(s=malloc(sizeof(STREAM))))
	{
		fuse_reply_err(req,EMFILE);
		return;
	}

	memset(s,0,sizeof(STREAM));
	s->flags=fi->flags;
	s->dev=dev;

	if((s->fd=dev->conf.dvr_open(dev->conf.user,dev->conf.dvr_pathname,
		fi->flags))==-1)
	{
		fuse_reply_err(req,errno);
		free(s);
		return;
	}

	pthread_mutex_lock(&dev->mtx);

	s->next=dev->s;
	dev->s=s;

	pthread_mutex_unlock(&dev->mtx);

	fi->direct_io=1;
	fi->keep_cache=0;
	fi->nonseekable=1;
	fi->fh=(uint64_t)s;

	fuse_reply_open(req,fi);
}

static void dvr_read(fuse_req_t req,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	ssize_t len;
	char bfr[131072];

	if((s->flags&O_ACCMODE)==O_WRONLY)
	{
		fuse_reply_err(req,EPERM);
		return;
	}

	if(!dev->conf.dvr_read)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if((len=dev->conf.dvr_read(dev->conf.user,s->fd,bfr,
		size>sizeof(bfr)?sizeof(bfr):size))==-1)
	{
		fuse_reply_err(req,errno);
		return;
	}

	fuse_reply_buf(req,bfr,len);
}

static void dvr_write(fuse_req_t req,const char *buf,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	ssize_t len;

	if((s->flags&O_ACCMODE)==O_RDONLY)
	{
		fuse_reply_err(req,EPERM);
		return;
	}

	if(!dev->conf.dvr_write)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if((len=dev->conf.dvr_write(dev->conf.user,s->fd,buf,size))==-1)
	{
		fuse_reply_err(req,errno);
		return;
	}

	fuse_reply_write(req,len);
}

static void dvr_flush(fuse_req_t req,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void dvr_release(fuse_req_t req,struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	STREAM **e;

	if(!dev->conf.dvr_close)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	dev->conf.dvr_close(dev->conf.user,s->fd);

	pthread_mutex_lock(&dev->mtx);

	for(e=&dev->s;*e;e=&(*e)->next)if(*e==s)
	{
		*e=s->next;
		free(s);
		break;
	}

	pthread_mutex_unlock(&dev->mtx);

	fuse_reply_err(req,0);
}

static void dvr_fsync(fuse_req_t req,int datasync,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void dvr_ioctl(fuse_req_t req,int cmd,void *arg,
	struct fuse_file_info *fi,unsigned flags,const void *in_buf,
	size_t in_bufsz,size_t out_bufsz)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;

	if(!dev->conf.dvr_ioctl)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(flags&FUSE_IOCTL_COMPAT)fuse_reply_err(req,ENOSYS);
	else switch(cmd)
	{
	case DMX_SET_BUFFER_SIZE:
		if(dev->conf.dvr_ioctl(dev->conf.user,s->fd,cmd,arg)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	default:
		fuse_reply_err(req,EINVAL);
		break;
	}
}

static void dvr_poll(fuse_req_t req,struct fuse_file_info *fi,
	struct fuse_pollhandle *ph)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct pollfd p;

	if(!dev->conf.dvr_poll)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	p.fd=s->fd;
	p.events=POLLIN;
	p.revents=0;

	dev->conf.dvr_poll(dev->conf.user,&p);

	fuse_reply_poll(req,p.revents);
	if(p.revents&&ph)fuse_lowlevel_notify_poll(ph);
}

static const struct cuse_lowlevel_ops dvr_ops=
{
	.init_done=dvr_post,
	.open=dvr_open,
	.read=dvr_read,
	.write=dvr_write,
	.flush=dvr_flush,
	.release=dvr_release,
	.fsync=dvr_fsync,
	.ioctl=dvr_ioctl,
	.poll=dvr_poll,
};

static void *dvrworker(void *data)
{
	struct cuse_info ci;
	DATA *dev=(DATA *)data;
	char devpath[PATH_MAX+9];
	const char *devarg[1]={devpath};
	int dummy_argc=1;
	char *dummy_argv[66]={""};
	struct fuse_args args=FUSE_ARGS_INIT(dummy_argc,dummy_argv);
	sigset_t set;

	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK,&set,NULL);

	if(fuse_opt_parse(&args,NULL,dvbtvd_opts,dvbtvd_args))goto out;
	if(fuse_opt_add_arg(&args, "-f"))goto out;

	sprintf(devpath,"DEVNAME=dvb/adapter%d/dvr0",dev->conf.adapter);
	memset(&ci,0,sizeof(ci));
	ci.dev_major=dev->conf.major;
	ci.dev_minor=dev->conf.minbase+2;
	ci.dev_info_argc=1;
	ci.dev_info_argv=devarg;
	ci.flags=CUSE_UNRESTRICTED_IOCTL;

	cuse_lowlevel_main(args.argc,args.argv,&ci,&dvr_ops,data);

out:	fuse_opt_free_args(&args);

	pthread_exit(NULL);
}

static void dmx_post(void *userdata)
{
	DATA *dev=(DATA *)userdata;
	char devpath[PATH_MAX];

	sprintf(devpath,"/dev/dvb/adapter%d/demux0",dev->conf.adapter);
	chown(devpath,dev->conf.owner,dev->conf.group);
	chmod(devpath,dev->conf.perms);
}

static void dmx_open(fuse_req_t req,struct fuse_file_info *fi)
{
	DATA *dev=fuse_req_userdata(req);
	STREAM *s;

	if(!dev)
	{
		fuse_reply_err(req,EINVAL);
		return;
	}

	if(!dev->conf.dmx_open)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(!(s=malloc(sizeof(STREAM))))
	{
		fuse_reply_err(req,EMFILE);
		return;
	}

	memset(s,0,sizeof(STREAM));
	s->flags=fi->flags;
	s->dev=dev;

	if((s->fd=dev->conf.dmx_open(dev->conf.user,dev->conf.dmx_pathname,
		fi->flags))==-1)
	{
		fuse_reply_err(req,errno);
		free(s);
		return;
	}

	pthread_mutex_lock(&dev->mtx);

	s->next=dev->s;
	dev->s=s;

	pthread_mutex_unlock(&dev->mtx);

	fi->direct_io=1;
	fi->keep_cache=0;
	fi->nonseekable=1;
	fi->fh=(uint64_t)s;

	fuse_reply_open(req,fi);
}

static void dmx_read(fuse_req_t req,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	ssize_t len;
	char bfr[131072];

	if((s->flags&O_ACCMODE)==O_WRONLY)
	{
		fuse_reply_err(req,EPERM);
		return;
	}

	if(!dev->conf.dmx_read)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if((len=dev->conf.dmx_read(dev->conf.user,s->fd,bfr,
		size>sizeof(bfr)?sizeof(bfr):size))==-1)
	{
		fuse_reply_err(req,errno);
		return;
	}

	fuse_reply_buf(req,bfr,len);
}

static void dmx_write(fuse_req_t req,const char *buf,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;

	if((s->flags&O_ACCMODE)==O_RDONLY)fuse_reply_err(req,EPERM);
	else fuse_reply_err(req,EOPNOTSUPP);
}

static void dmx_flush(fuse_req_t req,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void dmx_release(fuse_req_t req,struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	STREAM **e;

	if(!dev->conf.dmx_close)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	dev->conf.dmx_close(dev->conf.user,s->fd);

	pthread_mutex_lock(&dev->mtx);

	for(e=&dev->s;*e;e=&(*e)->next)if(*e==s)
	{
		*e=s->next;
		free(s);
		break;
	}

	pthread_mutex_unlock(&dev->mtx);

	fuse_reply_err(req,0);
}

static void dmx_fsync(fuse_req_t req,int datasync,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void dmx_ioctl(fuse_req_t req,int cmd,void *arg,
	struct fuse_file_info *fi,unsigned flags,const void *in_buf,
	size_t in_bufsz,size_t out_bufsz)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct iovec iov;
	union
	{
		uint16_t *pid;
		struct dmx_sct_filter_params *sctflt;
		struct dmx_pes_filter_params *pesflt;
		struct dmx_stc stc;
		uint16_t pespid[5];
	}u;

	if(!dev->conf.dmx_ioctl)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(flags&FUSE_IOCTL_COMPAT)fuse_reply_err(req,ENOSYS);
	else switch(cmd)
	{
	case DMX_START:
	case DMX_STOP:
		if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,NULL)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	case DMX_SET_BUFFER_SIZE:
		if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,arg)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	case DMX_ADD_PID:
	case DMX_REMOVE_PID:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(uint16_t);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.pid=(uint16_t *)in_buf;
			if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,u.pid)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case DMX_SET_FILTER:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dmx_sct_filter_params);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.sctflt=(struct dmx_sct_filter_params *)in_buf;
			if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,
				u.sctflt)==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case DMX_SET_PES_FILTER:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dmx_pes_filter_params);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.pesflt=(struct dmx_pes_filter_params *)in_buf;
			if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,
				u.pesflt)==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case DMX_GET_STC:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dmx_stc);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,&u.stc)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.stc,
				sizeof(struct dmx_stc));
		}
		break;

	case DMX_GET_PES_PIDS:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(u.pespid);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.dmx_ioctl(dev->conf.user,s->fd,cmd,
				u.pespid)==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,u.pespid,sizeof(u.pespid));
		}
		break;

	default:
		fuse_reply_err(req,EINVAL);
		break;
	}
}

static void dmx_poll(fuse_req_t req,struct fuse_file_info *fi,
	struct fuse_pollhandle *ph)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct pollfd p;

	if(!dev->conf.dmx_poll)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	p.fd=s->fd;
	p.events=POLLIN;
	p.revents=0;

	dev->conf.dmx_poll(dev->conf.user,&p);

	fuse_reply_poll(req,p.revents);
	if(p.revents&&ph)fuse_lowlevel_notify_poll(ph);
}

static const struct cuse_lowlevel_ops dmx_ops=
{
	.init_done=dmx_post,
	.open=dmx_open,
	.read=dmx_read,
	.write=dmx_write,
	.flush=dmx_flush,
	.release=dmx_release,
	.fsync=dmx_fsync,
	.ioctl=dmx_ioctl,
	.poll=dmx_poll,
};

static void *dmxworker(void *data)
{
	struct cuse_info ci;
	DATA *dev=(DATA *)data;
	char devpath[PATH_MAX+9];
	const char *devarg[1]={devpath};
	int dummy_argc=1;
	char *dummy_argv[66]={""};
	struct fuse_args args=FUSE_ARGS_INIT(dummy_argc,dummy_argv);
	sigset_t set;

	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK,&set,NULL);

	if(fuse_opt_parse(&args,NULL,dvbtvd_opts,dvbtvd_args))goto out;
	if(fuse_opt_add_arg(&args, "-f"))goto out;

	sprintf(devpath,"DEVNAME=dvb/adapter%d/demux0",dev->conf.adapter);
	memset(&ci,0,sizeof(ci));
	ci.dev_major=dev->conf.major;
	ci.dev_minor=dev->conf.minbase+1;
	ci.dev_info_argc=1;
	ci.dev_info_argv=devarg;
	ci.flags=CUSE_UNRESTRICTED_IOCTL;

	cuse_lowlevel_main(args.argc,args.argv,&ci,&dmx_ops,data);

out:	fuse_opt_free_args(&args);

	pthread_exit(NULL);
}

static void fe_post(void *userdata)
{
	DATA *dev=(DATA *)userdata;
	char devpath[PATH_MAX];

	sprintf(devpath,"/dev/dvb/adapter%d/frontend0",dev->conf.adapter);
	chown(devpath,dev->conf.owner,dev->conf.group);
	chmod(devpath,dev->conf.perms);
}

static void fe_open(fuse_req_t req,struct fuse_file_info *fi)
{
	DATA *dev=fuse_req_userdata(req);
	STREAM *s;

	if(!dev)
	{
		fuse_reply_err(req,EINVAL);
		return;
	}

	if(!dev->conf.fe_open)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if(!(s=malloc(sizeof(STREAM))))
	{
		fuse_reply_err(req,EMFILE);
		return;
	}

	memset(s,0,sizeof(STREAM));
	s->flags=fi->flags;
	s->dev=dev;

	if((s->fd=dev->conf.fe_open(dev->conf.user,dev->conf.fe_pathname,
		fi->flags))==-1)
	{
		fuse_reply_err(req,errno);
		free(s);
		return;
	}

	pthread_mutex_lock(&dev->mtx);

	s->next=dev->s;
	dev->s=s;

	pthread_mutex_unlock(&dev->mtx);

	fi->direct_io=1;
	fi->keep_cache=0;
	fi->nonseekable=1;
	fi->fh=(uint64_t)s;

	fuse_reply_open(req,fi);
}

static void fe_read(fuse_req_t req,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;

	if((s->flags&O_ACCMODE)==O_WRONLY)fuse_reply_err(req,EPERM);
	else fuse_reply_err(req,EOPNOTSUPP);
}

static void fe_write(fuse_req_t req,const char *buf,size_t size,off_t off,
	struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;

	if((s->flags&O_ACCMODE)==O_RDONLY)fuse_reply_err(req,EPERM);
	else fuse_reply_err(req,EOPNOTSUPP);
}

static void fe_flush(fuse_req_t req,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void fe_release(fuse_req_t req,struct fuse_file_info *fi)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	STREAM **e;

	if(!dev->conf.fe_close)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	dev->conf.fe_close(dev->conf.user,s->fd);

	pthread_mutex_lock(&dev->mtx);

	for(e=&dev->s;*e;e=&(*e)->next)if(*e==s)
	{
		*e=s->next;
		free(s);
		break;
	}

	pthread_mutex_unlock(&dev->mtx);

	fuse_reply_err(req,0);
}

static void fe_fsync(fuse_req_t req,int datasync,struct fuse_file_info *fi)
{
	fuse_reply_err(req,EOPNOTSUPP);
}

static void fe_ioctl(fuse_req_t req,int cmd,void *arg,
	struct fuse_file_info *fi,unsigned flags,const void *in_buf,
	size_t in_bufsz,size_t out_bufsz)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct iovec iov;
	union
	{
		struct dvb_frontend_info info;
		fe_status_t status;
		uint32_t u32;
		uint16_t u16;
		struct dvb_diseqc_master_cmd *cmd;
		struct dvb_diseqc_slave_reply reply;
		struct dvb_frontend_parameters *pin;
		struct dvb_frontend_parameters pout;
		struct dvb_frontend_event event;
	}u;
	struct dtv_properties props;

	if(!dev->conf.fe_ioctl)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	if((s->flags&O_ACCMODE)==O_RDONLY&&(_IOC_DIR(cmd)!=_IOC_READ||
		cmd==FE_GET_EVENT||cmd==FE_DISEQC_RECV_SLAVE_REPLY))
	{
		fuse_reply_err(req,EPERM);
		return;
	}

	if(flags&FUSE_IOCTL_COMPAT)fuse_reply_err(req,ENOSYS);
	else switch(cmd)
	{
	case FE_SET_PROPERTY:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dtv_properties);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else if(in_bufsz==sizeof(struct dtv_properties))
		{
			props=*(struct dtv_properties *)in_buf;
			if(!props.num||props.num>DTV_IOCTL_MAX_MSGS)
				fuse_reply_err(req,EINVAL);
			else
			{
				iov.iov_base=props.props;
				iov.iov_len=sizeof(struct dtv_property)
					*props.num;
				fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
			}
		}
		else
		{
			props.props=(struct dtv_property *)in_buf;
			props.num=in_bufsz/sizeof(struct dtv_property);
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&props)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case FE_GET_PROPERTY:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dtv_properties);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else if(in_bufsz==sizeof(struct dtv_properties)&&!out_bufsz)
		{
			props=*(struct dtv_properties *)in_buf;
			if(!props.num||props.num>DTV_IOCTL_MAX_MSGS)
				fuse_reply_err(req,EINVAL);
			else
			{
				iov.iov_base=props.props;
				iov.iov_len=sizeof(struct dtv_property)
					*props.num;
				fuse_reply_ioctl_retry(req,&iov,1,&iov,1);
			}
		}
		else if(in_bufsz&&in_bufsz==out_bufsz)
		{
			props.props=(struct dtv_property *)in_buf;
			props.num=out_bufsz/sizeof(struct dtv_property);
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&props)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,in_buf,out_bufsz);
		}
		else fuse_reply_err(req,ENODATA);
		break;

	case FE_GET_INFO:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_frontend_info);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&u.info)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.info,
				sizeof(struct dvb_frontend_info));
		}
		break;

	case FE_READ_STATUS:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(fe_status_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,
				&u.status)==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.status,
				sizeof(fe_status_t));
		}
		break;

	case FE_READ_BER:
	case FE_READ_UNCORRECTED_BLOCKS:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(uint32_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&u.u32)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.u32,
				sizeof(uint32_t));
		}
		break;

	case FE_READ_SIGNAL_STRENGTH:
	case FE_READ_SNR:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(uint16_t);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&u.u16)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.u16,
				sizeof(uint16_t));
		}
		break;

	case FE_DISEQC_RESET_OVERLOAD:
		if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,NULL)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	case FE_DISEQC_SEND_MASTER_CMD:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_diseqc_master_cmd);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.cmd=(struct dvb_diseqc_master_cmd *)in_buf;
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,u.cmd)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case FE_DISEQC_SEND_BURST:
	case FE_SET_TONE:
	case FE_SET_VOLTAGE:
	case FE_DISHNETWORK_SEND_LEGACY_CMD:
	case FE_ENABLE_HIGH_LNB_VOLTAGE:
	case FE_SET_FRONTEND_TUNE_MODE:
		if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,arg)==-1)
			fuse_reply_err(req,errno);
		else fuse_reply_ioctl(req,0,NULL,0);
		break;

	case FE_DISEQC_RECV_SLAVE_REPLY:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_diseqc_slave_reply);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&u.reply)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.reply,
				sizeof(struct dvb_diseqc_slave_reply));
		}
		break;

	case FE_SET_FRONTEND:
		if(!in_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_frontend_parameters);
			fuse_reply_ioctl_retry(req,&iov,1,NULL,0);
		}
		else
		{
			u.pin=(struct dvb_frontend_parameters *)in_buf;
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,u.pin)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,NULL,0);
		}
		break;

	case FE_GET_FRONTEND:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_frontend_parameters);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&u.pout)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.pout,
				sizeof(struct dvb_frontend_parameters));
		}
		break;

	case FE_GET_EVENT:
		if(!out_bufsz)
		{
			iov.iov_base=arg;
			iov.iov_len=sizeof(struct dvb_frontend_event);
			fuse_reply_ioctl_retry(req,NULL,0,&iov,1);
		}
		else
		{
			if(dev->conf.fe_ioctl(dev->conf.user,s->fd,cmd,&u.pout)
				==-1)fuse_reply_err(req,errno);
			else fuse_reply_ioctl(req,0,&u.event,
				sizeof(struct dvb_frontend_event));
		}
		break;


	default:
		fuse_reply_err(req,EINVAL);
		break;
	}
}

static void fe_poll(fuse_req_t req,struct fuse_file_info *fi,
	struct fuse_pollhandle *ph)
{
	STREAM *s=(STREAM *)fi->fh;
	DATA *dev=s->dev;
	struct pollfd p;

	if(!dev->conf.fe_poll)
	{
		fuse_reply_err(req,EOPNOTSUPP);
		return;
	}

	p.fd=s->fd;
	p.events=POLLIN;
	p.revents=0;

	dev->conf.fe_poll(dev->conf.user,&p);

	fuse_reply_poll(req,p.revents);
	if(p.revents&&ph)fuse_lowlevel_notify_poll(ph);
}

static const struct cuse_lowlevel_ops fe_ops=
{
	.init_done=fe_post,
	.open=fe_open,
	.read=fe_read,
	.write=fe_write,
	.flush=fe_flush,
	.release=fe_release,
	.fsync=fe_fsync,
	.ioctl=fe_ioctl,
	.poll=fe_poll,
};

static void *feworker(void *data)
{
	struct cuse_info ci;
	DATA *dev=(DATA *)data;
	char devpath[PATH_MAX+9];
	const char *devarg[1]={devpath};
	int dummy_argc=1;
	char *dummy_argv[66]={""};
	struct fuse_args args=FUSE_ARGS_INIT(dummy_argc,dummy_argv);
	sigset_t set;

	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK,&set,NULL);

	if(fuse_opt_parse(&args,NULL,dvbtvd_opts,dvbtvd_args))goto out;
	if(fuse_opt_add_arg(&args, "-f"))goto out;

	sprintf(devpath,"DEVNAME=dvb/adapter%d/frontend0",dev->conf.adapter);
	memset(&ci,0,sizeof(ci));
	ci.dev_major=dev->conf.major;
	ci.dev_minor=dev->conf.minbase;
	ci.dev_info_argc=1;
	ci.dev_info_argv=devarg;
	ci.flags=CUSE_UNRESTRICTED_IOCTL;

	cuse_lowlevel_main(args.argc,args.argv,&ci,&fe_ops,data);

out:	fuse_opt_free_args(&args);

	pthread_exit(NULL);
}

void *dvbcuse_create(DVBCUSE_DEVICE *config)
{
	DATA *dev;
	int i;
	struct stat stb;
	char bfr[PATH_MAX];

	if(!config)goto err1;

	if(config->adapter<0||config->adapter>255||config->major<0||
		config->major>0x7fff||config->minbase<0||config->minbase>0x7fff)
			goto err1;

	if(config->minbase&7)goto err1;

	if(stat("/dev/cuse",&stb)||!S_ISCHR(stb.st_mode)||
		access("/dev/cuse",R_OK|W_OK))goto err1;

	if(config->fe_enabled)
	{
		sprintf(bfr,"/dev/dvb/adapter%d/frontend0",config->adapter);
		if(!stat(bfr,&stb))goto err1;
	}

	if(config->dmx_enabled)
	{
		sprintf(bfr,"/dev/dvb/adapter%d/demux0",config->adapter);
		if(!stat(bfr,&stb))goto err1;
	}

	if(config->dvr_enabled)
	{
		sprintf(bfr,"/dev/dvb/adapter%d/dvr0",config->adapter);
		if(!stat(bfr,&stb))goto err1;
	}

	if(config->ca_enabled)
	{
		sprintf(bfr,"/dev/dvb/adapter%d/ca0",config->adapter);
		if(!stat(bfr,&stb))goto err1;
	}

	if(config->net_enabled)
	{
		sprintf(bfr,"/dev/dvb/adapter%d/net0",config->adapter);
		if(!stat(bfr,&stb))goto err1;
	}

	if(!(dev=malloc(sizeof(DATA))))goto err1;
	memset(dev,0,sizeof(DATA));
	dev->conf=*config;

	if(pthread_mutex_init(&dev->mtx,NULL))goto err2;

	for(i=0;i<5;i++)switch(i)
	{
	case 0:	if(dev->conf.fe_enabled)
			if(pthread_create(&dev->th[0],NULL,feworker,dev))
				goto err3;
		break;

	case 1:	if(dev->conf.dmx_enabled)
			if(pthread_create(&dev->th[1],NULL,dmxworker,dev))
				goto err3;
		break;

	case 2:	if(dev->conf.dvr_enabled)
			if(pthread_create(&dev->th[2],NULL,dvrworker,dev))
				goto err3;
		break;

	case 3:	if(dev->conf.ca_enabled)
			if(pthread_create(&dev->th[3],NULL,caworker,dev))
				goto err3;
		break;

	case 4:	if(dev->conf.net_enabled)
			if(pthread_create(&dev->th[4],NULL,networker,dev))
				goto err3;
		break;

	}

	return dev;

err3:	for(i--;i>=0;i--)switch(i)
	{
	case 3:	if(!dev->conf.ca_enabled)break;
		pthread_cancel(dev->th[i]);
		pthread_join(dev->th[i],NULL);
		break;
	case 2:	if(!dev->conf.dvr_enabled)break;
		pthread_cancel(dev->th[i]);
		pthread_join(dev->th[i],NULL);
		break;
	case 1:	if(!dev->conf.dmx_enabled)break;
		pthread_cancel(dev->th[i]);
		pthread_join(dev->th[i],NULL);
		break;
	case 0:	if(!dev->conf.fe_enabled)break;
		pthread_cancel(dev->th[i]);
		pthread_join(dev->th[i],NULL);
		break;
	}
	pthread_mutex_destroy(&dev->mtx);
err2:	free(dev);
err1:	return NULL;
}

void dvbcuse_destroy(void *ctx)
{
	int i;
	DATA *dev=(DATA *)ctx;

	if(!dev)return;

	for(i=5;i>=0;i--)switch(i)
	{
	case 4:	if(!dev->conf.net_enabled)break;
		pthread_cancel(dev->th[i]);
		break;

	case 3:	if(!dev->conf.ca_enabled)break;
		pthread_cancel(dev->th[i]);
		break;

	case 2:	if(!dev->conf.dvr_enabled)break;
		pthread_cancel(dev->th[i]);
		break;

	case 1:	if(!dev->conf.dmx_enabled)break;
		pthread_cancel(dev->th[i]);
		break;

	case 0:	if(!dev->conf.fe_enabled)break;
		pthread_cancel(dev->th[i]);
		break;
	}

	for(i=5;i>=0;i--)switch(i)
	{
	case 4:	if(!dev->conf.net_enabled)break;
		pthread_join(dev->th[i],NULL);
		break;

	case 3:	if(!dev->conf.ca_enabled)break;
		pthread_join(dev->th[i],NULL);
		break;

	case 2:	if(!dev->conf.dvr_enabled)break;
		pthread_join(dev->th[i],NULL);
		break;

	case 1:	if(!dev->conf.dmx_enabled)break;
		pthread_join(dev->th[i],NULL);
		break;

	case 0:	if(!dev->conf.fe_enabled)break;
		pthread_join(dev->th[i],NULL);
		break;
	}

	pthread_mutex_destroy(&dev->mtx);
	free(dev);
}
