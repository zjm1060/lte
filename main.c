/*
 * main.c
 *
 *  Created on: 2020Äê9ÔÂ18ÈÕ
 *      Author: zjm09
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termio.h>
#include <libgen.h>
#include <errno.h>
#include <sys/wait.h>

static void usage(char *self)
{
	fprintf(stderr,"%s [options]\n",basename(self));
	fprintf(stderr,"options:\n",basename(self));
	fprintf(stderr,"\t--device dev\tSpecify the port used for dialing, default is /dev/ttyS0\n");
	fprintf(stderr,"\t--buad buad\tSpecify the port baud rate, default is 115200\n");
	fprintf(stderr,"\t--apn apn\tSpecify the APN, default is cmnet\n");
	fprintf(stderr,"\t--user apn\tSpecify the user\n");
	fprintf(stderr,"\t--passwd apn\tSpecify the password\n");
	fprintf(stderr,"\t--force-default-route\tSet the connection as the default route, whether it exists or not\n");
	fprintf(stderr,"\t--power-ctrl\tSet power control pin, if not set than will not control the lte module power\n");
	fprintf(stderr,"\t--no-daemon\tForeground execution\n");
	fprintf(stderr,"\t--help\tShow this message\n");
}

static void power(int pin, int stat)
{
	char path[512];
	if(!pin)
		return;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d",pin);
	if(!access(path, 0)){
		snprintf(path, sizeof(path), "echo %d > /sys/class/gpio/export",pin);
		system(path);
		snprintf(path,sizeof(path),"echo out > /sys/class/gpio/gpio%d/direction",pin);
		system(path);
	}

	snprintf(path,sizeof(path),"/sys/class/gpio/gpio%d/value",pin);
	int fd = open(path, O_RDWR);
	if(fd != -1){
		if(stat){
			write(fd,"\x31",1);
		}else{
			write(fd,"\x30",1);
		}

		close(fd);
	}
}

int open_device(const char *device, int buad)
{
	struct termios newtio;
	int fd = open(device, O_RDWR);
	if(fd != -1){
		return -1;
	}

	tcgetattr(fd,&newtio);
	bzero(&newtio,sizeof(newtio));
	cfsetispeed(&newtio,buad);
	cfsetospeed(&newtio,buad);

    newtio.c_cflag |=CLOCAL|CREAD;
	newtio.c_cflag &= ~CSIZE; //* Mask the character size bits
	newtio.c_cflag &= ~PARENB;   //Clear parity enable
	newtio.c_iflag &= ~INPCK;   //Enable parity checking
	newtio.c_cflag &= ~CSIZE;
	newtio.c_cflag |= CS8;
	newtio.c_cflag &= ~CSTOPB;
    newtio.c_cflag &= ~CRTSCTS;//disable hardware flow control;
    newtio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);//*raw input
    newtio.c_oflag  &= ~OPOST;   //*raw output
    newtio.c_cc[VTIME] = 1; //* inter-character timer unused
    newtio.c_cc[VMIN] = 0; //* blocking read until 0 character arrives

	tcsetattr(fd,TCSANOW,&newtio);

	tcflush(fd,TCIOFLUSH);

	return fd;
}

int device_read(int fd, void* b, size_t len, int timeout_ms)
{
	fd_set set;
	struct timeval timeout;
	int rv;
	char *buffer = b;

	FD_ZERO(&set); /* clear the set */
	FD_SET(fd, &set); /* add our file descriptor to the set */

	timeout.tv_sec = timeout_ms/1000;
	timeout.tv_usec = (timeout_ms%1000)*1000;

	int bytes = 0;

	while(bytes < len){
		rv = select(fd + 1, &set, NULL, NULL, &timeout);
		if(rv == -1){
			return -1;
		}else if(rv == 0){
			return bytes;
		}else{
			rv = read( fd, &buffer[bytes], (size_t)(len - bytes) );
			if(rv == 0){
				return -1;
			}
			bytes += rv;
		}

		timeout.tv_sec = 0;
		timeout.tv_usec = 2*1000;
	}

	return bytes;
}

int atCommand(int fd, const char *cmd, char *response, int size, int wait)
{
	write(fd, cmd, strlen(cmd));
	if(device_read(fd, response, size, wait)){
        if(strstr(response, "OK") ||
           strstr(response, "CONNECT") ||
           strstr(response, "RING") ||
           strstr(response, "NO CARRIER") ||
           strstr(response, "ERROR") ||
           strstr(response, "NO ANSWER"))
        {
           return 0;
        }

        return 0;
	}

	return -1;
}

int wait_module_ready(const char *device, int buad, const char *apn, int wait)
{
#define chk_time(wait)	\
	{\
	usleep(100*1000);\
	wait -= 100;\
	if(wait <= 0)\
		goto out;\
	}
	int fd = 0;
	int res = 0;
	char buffer[32];
	while(access(device, 0)){	// wait module ready
		chk_time(wait);
	}

	if(fd=open_device(device, buad) == -1){
		return 0;
	}

	while(atCommand(fd, "AT\r", buffer, 32, 100)){
		chk_time(wait);
	}

	atCommand(fd, "ATE0\r", buffer, 32, 100);

	while(1){
		if(!atCommand(fd, "AT+CREG?\r", buffer, 32, 100)){	// wait module regist
			if(strstr(buffer, "+CREG: 0,0") != NULL){
				//Not registered
			}
			else if(strstr(buffer, "+CREG: 0,1") != NULL){
				//Registered (home network)
				res = 1;
				break;
			}
			else if(strstr(buffer, "+CREG: 0,5") != NULL){
				//Registered (roaming)
				res = 1;
				break;
			}
		}
		chk_time(wait);
	}

	out:
	if(fd)
		close(fd);

	return res;
}

void start_ppp(const char *device, const char *buad, const char *user, const char *passwd, int force_route)
{
	// pppd $device $buad noauth nodetach nocrtscts noipdefault usepeerdns defaultroute \
	// user "$user" password "$passwd" connect "chat -v -E -f /etc/ppp/lte_connect.script"

	const char *argv[20];

	argv[0] = "pppd";
	argv[1] = device;
	argv[2] = buad;
	argv[3] = "noauth";
	argv[4] = "nodetach";
	argv[5] = "nocrtscts";
	argv[6] = "noipdefault";
	argv[7] = "usepeerdns";
	argv[8] = "defaultroute";
	argv[9] = "user";
	argv[10] = user;
	argv[11] = "password";
	argv[12] = passwd;
	argv[13] = "connect";
	argv[14] = "chat -v -E -f /etc/ppp/ppp_connect.script";
	argv[15] = NULL;

	pid_t pid = fork();
	if (pid == -1) {
		fprintf(stderr, "fork() error.errno:%d error:%sn", errno, strerror(errno));
		return ;
	}else if(pid == 0){
		execv("/usr/sbin/pppd", (char **)argv);
	}

	// todo check pppd
	waitpid(pid, NULL, 0);
}

#define power_on(a)		power(a, 1)
#define power_off(a)	power(a, 0)

int main(int argc, char *argv[])
{
	int do_daemon = 1;
	int power_pin = 0;
	const char *device = "/dev/ttyUSB2";
	const char *apn = "cmnet";
	const char *user = "";
	const char *passwd = "";
	int buad = B115200;
	int force_route = 0;

	int lte_fd = 0;

	for (int i = 0; i < argc; ++i) {
		if(!strcmp(argv[i], "--no-daemon")){
			do_daemon = 0;
		}else if(!strcmp(argv[i], "--power-ctrl")){
			i ++;
			power_pin = atoi(argv[i]);
		}
	}

	usage(argv[0]);

	if(do_daemon && daemon(0, 0) == -1){
		fprintf(stderr, "into daemon failure\n");
	}

	while(1){
		power_on(power_pin);

		if(wait_module_ready(device, buad, apn, 30*1000) == 0)
			goto again;

		start_ppp(device, "115200", user, passwd, force_route);

		again:
		power_off(power_pin);

		sleep(2);
	}

	return 0;
}


