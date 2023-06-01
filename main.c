/*
 * main.c
 *
 *  Created on: 2020.9.18
 *      Author: zjm09
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <termio.h>
#include <libgen.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <gpiod.h>
#include <syslog.h>

static int ipv6 = 0;
static int use_gpiod = 0;
static char gpio_chip[32];
static uint32_t gpio_line;

static void usage(char *self)
{
	fprintf(stderr, "%s [options]\n", basename(self));
	fprintf(stderr, "options:\n", basename(self));
	fprintf(stderr, "\t--device dev\tSpecify the port used for dialing\n\t\t\tdefault is /dev/ttyS0\n");
	fprintf(stderr, "\t--buad buad\tSpecify the port baud rate, default is 115200\n");
	fprintf(stderr, "\t--apn xxx\tSpecify the APN, default is cmnet\n");
	fprintf(stderr, "\t--user xxx\tSpecify the user\n");
	fprintf(stderr, "\t--passwd xxx\tSpecify the password\n");
	fprintf(stderr, "\t--ipv6\tSupport IPv6\n");
	fprintf(stderr, "\t--power-ctrl n\tSet power control pin\n\t\t\tif not set than will not control the lte module power\n");
	fprintf(stderr, "\t--unit n\tSets the ppp unit number for outbound connections\n\t\t\tdefault is 0\n");
	fprintf(stderr, "\t--no-daemon\tForeground execution\n");
	fprintf(stderr, "\t--no-dns\tForeground execution\n");
	fprintf(stderr, "\t--help\tShow this message\n");
}

int r_str_isnumber(const char *str)
{
	if (!str || (!isdigit(*str) && *str != '-')) {
		return 0;
	}

	for (str++; *str; str++) {
		if (!isdigit (*str)) {
			return 0;
		}
	}

	return 1;
}

static void power(int pin, int stat)
{
	char path[512];
	if (!pin)
		return;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
	if (access(path, 0))
	{
		snprintf(path, sizeof(path), "echo %d > /sys/class/gpio/export", pin);
		system(path);
		snprintf(path, sizeof(path), "echo out > /sys/class/gpio/gpio%d/direction", pin);
		system(path);
	}

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
	int fd = open(path, O_RDWR);
	if (fd != -1)
	{
		if (stat)
		{
			write(fd, "\x31", 1);
		}
		else
		{
			write(fd, "\x30", 1);
		}

		close(fd);
	}
}

int open_device(const char *device, int buad)
{
	struct termios newtio;
	int fd = open(device, O_RDWR);
	if (fd == -1)
	{
		return -1;
	}

	tcgetattr(fd, &newtio);
	bzero(&newtio, sizeof(newtio));
	cfsetispeed(&newtio, buad);
	cfsetospeed(&newtio, buad);

	newtio.c_cflag |= CLOCAL | CREAD;
	newtio.c_cflag &= ~CSIZE;  //* Mask the character size bits
	newtio.c_cflag &= ~PARENB; //Clear parity enable
	newtio.c_iflag &= ~INPCK;  //Enable parity checking
	newtio.c_cflag &= ~CSIZE;
	newtio.c_cflag |= CS8;
	newtio.c_cflag &= ~CSTOPB;
	newtio.c_cflag &= ~CRTSCTS;						   //disable hardware flow control;
	newtio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); //*raw input
	newtio.c_oflag &= ~OPOST;						   //*raw output
	newtio.c_cc[VTIME] = 1;							   //* inter-character timer unused
	newtio.c_cc[VMIN] = 0;							   //* blocking read until 0 character arrives

	tcsetattr(fd, TCSANOW, &newtio);

	tcflush(fd, TCIOFLUSH);

	return fd;
}

int device_read(int fd, void *b, size_t len, int timeout_ms)
{
	fd_set set;
	struct timeval timeout;
	int rv;
	char *buffer = b;

	FD_ZERO(&set);	  /* clear the set */
	FD_SET(fd, &set); /* add our file descriptor to the set */

	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;

	int bytes = 0;

	while (bytes < len)
	{
		rv = select(fd + 1, &set, NULL, NULL, &timeout);
		if (rv == -1)
		{
			return -1;
		}
		else if (rv == 0)
		{
			return bytes;
		}
		else
		{
			rv = read(fd, &buffer[bytes], (size_t)(len - bytes));
			if (rv == 0)
			{
				return -1;
			}
			bytes += rv;
		}

		timeout.tv_sec = 0;
		timeout.tv_usec = 2 * 1000;
	}

	return bytes;
}

int atCommand(int fd, const char *cmd, char *response, int size, int wait)
{
//	fprintf(stderr, "send:%s\n", cmd);
	memset(response, 0, size);
	write(fd, cmd, strlen(cmd));
	if (device_read(fd, response, size, wait))
	{
//		fprintf(stderr, "recv:%s\n", response);
		if (strstr(response, "OK") ||
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

int wait_module_ready(int ins, const char *device, int buad, const char *apn, int wait)
{
#define chk_time(wait)      \
	{                       \
		usleep(2000 * 1000); \
		wait -= 2000;        \
		if (wait <= 0)      \
			goto out;       \
	}
	int fd = 0;
	int res = 0;
	char buffer[128];
	snprintf(buffer, sizeof(buffer), "/tmp/LTE%d.info", ins);
	FILE *fp = fopen(buffer,"w+");

	while (access(device, 0))
	{ // wait module ready
		chk_time(wait);
	}

	if ((fd = open_device(device, buad)) == -1)
	{
		return 0;
	}

	while (atCommand(fd, "AT\r", buffer, 128, 100))
	{
		chk_time(wait);
	}

	atCommand(fd, "ATE0\r", buffer, 128, 100);
// AT+QCFG="nwscanmode",0,1
//	atCommand(fd, "AT+QCFG=\"nwscanmode\",1,1\r", buffer, 128, 2000);

	while(1)
	{
		atCommand(fd, "AT+CPIN?\r", buffer, 128, 100);
		if (strstr(buffer, "READY") != NULL){
			syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"CPIN READY\n");
			break;
		}

		chk_time(wait);
	}

	fprintf(fp, "{\n");

	while(1){
		if(!atCommand(fd, "AT+CGSN\r", buffer, 128, 100)){
			char cgsn[32];

			memset(cgsn, 0, sizeof(cgsn));
			sscanf(buffer, "%*[^0-9]%30s", cgsn);
			if(cgsn[0] >= '0' && cgsn[0] <= '9' && strlen(cgsn) > 5){
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"IMEI:%s\n",cgsn);
				fprintf(fp, "\"IMEI\":\"%s\",\n",cgsn);
				break;
			}
		}
		chk_time(wait);
	}

	while(1){
		if(!atCommand(fd, "AT+CIMI\r", buffer, 128, 100)){
			char cimi[32];

			memset(cimi, 0, sizeof(cimi));
			sscanf(buffer, "%*[^0-9]%30s", cimi);
			if(cimi[0] >= '0' && cimi[0] <= '9' && strlen(cimi) > 5){
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"IMSI:%s\n",cimi);
				fprintf(fp, "\"IMSI\":\"%s\",\n",cimi);
				break;
			}
		}
		chk_time(wait);
	}

	while(1){
		if(!atCommand(fd, "AT+CCID\r", buffer, 128, 100)){
			char ccid[32];

			memset(ccid, 0, sizeof(ccid));
			sscanf(buffer, "%*[^0-9]%[^\"\n\r]", ccid);
			if(ccid[0] >= '0' && ccid[0] <= '9' && strlen(ccid) > 5){
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"CCID:%s\n",ccid);
				fprintf(fp, "\"CCID\":\"%s\",\n",ccid);
				break;
			}
		}
		chk_time(wait);
	}

	if(!atCommand(fd, "ATI\r", buffer, 128, 100)){
		char version[32];

		char *ver = strstr(buffer, "Revision:");
		if(ver){
			sscanf(ver, "Revision: %30s", version);
			syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"version:%s\n",version);
			fprintf(fp, "\"VER\":\"%s\"\n",version);
		}
		chk_time(wait);
	}

	fprintf(fp, "}\n");

	fflush(fp);

	while (1)
	{
		if (!atCommand(fd, "AT+CREG?\r", buffer, 32, 100))
		{ // wait module regist
//			printf("%s\n",buffer);
			if (strstr(buffer, "+CREG: 0,0") != NULL)
			{
				//Not registered
			}
			else if (strstr(buffer, "+CREG: 0,1") != NULL)
			{
				//Registered (home network)
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"Registered (home network)\n");
				res = 1;
				break;
			}
			else if (strstr(buffer, "+CREG: 0,5") != NULL)
			{
				//Registered (roaming)
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"Registered (roaming)\n");
				res = 1;
				break;
			}
		}

		chk_time(wait);
	}

	while(1){
		if(!atCommand(fd, "AT+COPS?\r", buffer, 128, 100)){
			char cops[32];
			char *ops;
			memset(cops, 0, sizeof(cops));
			ops = strstr(buffer, "+COPS:");
			if(ops){
				sscanf(ops, "+COPS:%*[^,],%*[^,],\"%[^\"]", cops);
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"COPS:%s\n",cops);
				fprintf(fp, "COPS:%s\n",cops);
			}
			break;
		}
	}

out:
	fclose(fp);
	if (fd)
		close(fd);

	return res;
}

static int child_runing = 0;
void signal_hander(int sig)
{
	int status;
	pid_t pid;

//	printf("sig:%d\n",sig);
	if ((pid = waitpid(0, &status, WNOHANG)) > 0) {
//    while ((pid = waitpid(0, &status, WNOHANG)) > 0) {
		if (WIFEXITED(status))
			printf("------------child %d exit %d\n", pid, WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			printf("child %d cancel signal %d\n", pid, WTERMSIG(status));

		child_runing = 0;
	}
}

char *getip(const char *name)
{
	struct ifreq temp;
	struct sockaddr_in *myaddr;
	static char ip_buf[64];
	int fd = 0;
	int ret = -1;
	strcpy(temp.ifr_name, name);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		return "0.0.0.0";
	}
	ret = ioctl(fd, SIOCGIFFLAGS, &temp);
	if (ret < 0){
		close(fd);
		return "0.0.0.0";
	}

	if(temp.ifr_flags & IFF_UP){
		ret = ioctl(fd, SIOCGIFADDR, &temp);
		close(fd);
		if (ret < 0)
			return "0.0.0.0";
		myaddr = (struct sockaddr_in *) &(temp.ifr_addr);
		strcpy(ip_buf, inet_ntoa(myaddr->sin_addr));
		return ip_buf;
	}else{
		close(fd);
	}

	return "0.0.0.0";
}

int getStatus(const char *name)
{
	char file[256];

	snprintf(file, sizeof(file), "/sys/class/net/%s/flags", name);

	if(!access(file, 0)){

	}

	return 0;
}

void start_ppp(const char *device, const char *buad, int uint, const char *apn, const char *user, const char *passwd, int use_dns)
{
	// pppd $device $buad noauth nodetach nocrtscts noipdefault usepeerdns defaultroute \
	// user "$user" password "$passwd" connect "chat -v -E -f /etc/ppp/lte_connect.script"
	char unit[2];
	const char *argv[20];
	const char **arg = &argv[0];

	unit[0] = '0'+uint;
	unit[1] = 0;

	*arg++ = "pppd";
	*arg++ = device;
	*arg++ = buad;
	*arg++ = "noauth";
	*arg++ = "nodetach";
	*arg++ = "nocrtscts";
	*arg++ = "noipdefault";
	if(use_dns)
		*arg++ = "usepeerdns";
	*arg++ = "user";
	*arg++ = user;
	*arg++ = "password";
	*arg++ = passwd;
	*arg++ = "connect";
	*arg++ = "chat -v -E -f /etc/ppp/ppp.script";
	if(ipv6)
		*arg++ = "+ipv6";
	if(uint >= 0){
		*arg++ = "unit";
		*arg++ = unit;
	}
	*arg++ = "nodefaultroute";
	*arg++ = NULL;

	setenv("PPP_APN",apn,1);

	signal(SIGCHLD,signal_hander);
	child_runing = 1;

	pid_t pid = fork();
	if (pid == -1)
	{
		fprintf(stderr, "fork() error.errno:%d error:%sn", errno, strerror(errno));
		return;
	}
	else if (pid == 0)
	{
		execv("/usr/sbin/pppd", (char **)argv);
	}
	else
	{
		sleep(5);

		while(child_runing){
			char ifpath[1024];
			int fd = 0;
			sleep(5);
			snprintf(ifpath, sizeof(ifpath), "/sys/class/net/ppp%d/carrier", uint);
//			char *ip =  getip(ifname);
//			printf("ip:%s\n",ip);
//			if(!strcmp("0.0.0.0",ip)){
//				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO), "%s is down\n", ifname);
//				kill(pid, SIGTERM);
//			}

			if(access(ifpath, F_OK | R_OK)){
				syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO), "ppp%d is down\n", uint);
				kill(pid, SIGTERM);
			}
		}

		syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO), "pppd is exit,restart it\n");
		signal(SIGCHLD, SIG_IGN);
	}
}

#define power_on(a) (power_pin)?\
			((use_gpiod)?gpiod_ctxless_set_value(gpio_chip, gpio_line, 1, 0, "lte_power", NULL, NULL):power(a, 1)):\
			0
#define power_off(a) (power_pin)?\
			((use_gpiod)?gpiod_ctxless_set_value(gpio_chip, gpio_line, 0, 0, "lte_power", NULL, NULL):power(a, 0)):\
			0

#define P(q,s) case q:s = B##q;break;
#define i2b(b)	\
	({\
	int s=B1200;\
	switch(b){\
		P(1200,s)\
		P(2400,s)\
		P(4800,s)\
		P(9600,s)\
		P(19200,s)\
		P(38400,s)\
		P(57600,s)\
		P(115200,s)\
		P(230400,s)\
	}\
	s;})

int main(int argc, char *argv[])
{
	int do_daemon = 1;
	int power_pin = 0;
	const char *device = "/dev/ttyUSB1";
	const char *apn = "cmnet";
	const char *user = "";
	const char *passwd = "";
	int buad = B115200;
	int wait = 120*1000;
	int usepeerdns = 1;
	char *buad_str = "115200";
	int ins = 0;
//	int force_route = 0;

	int lte_fd = 0;

	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "--no-daemon"))
		{
			do_daemon = 0;
		}
		else if (!strcmp(argv[i], "--help"))
		{
			usage(argv[0]);
			exit(0);
		}
		else if (!strcmp(argv[i], "--no-dns"))
		{
			usepeerdns = 0;
		}
		else if (!strcmp(argv[i], "--power-ctrl"))
		{
			i++;
			if(r_str_isnumber(argv[i])){
				power_pin = atoi(argv[i]);
			}else{
				use_gpiod = 1;
				power_pin = 1;
				if(gpiod_ctxless_find_line(argv[i], gpio_chip, sizeof(gpio_chip), &gpio_line) < 0){

				}
			}
		}
		else if (!strcmp(argv[i], "--device"))
		{
			i++;
			device = argv[i];
		}
		else if (!strcmp(argv[i], "--apn"))
		{
			i++;
			apn = argv[i];
		}
		else if (!strcmp(argv[i], "--user"))
		{
			i++;
			user = argv[i];
		}
		else if (!strcmp(argv[i], "--passwd"))
		{
			i++;
			passwd = argv[i];
		}
		else if (!strcmp(argv[i], "--ipv6")){
			ipv6 = 1;
		}
		else if (!strcmp(argv[i], "--unit"))
		{
			i++;
			ins = atoi(argv[i]);
		}
		else if (!strcmp(argv[i], "--buad"))
		{
			i ++;
			int b = atoi(argv[i]);
			buad_str = argv[i];
			buad = i2b(b);
		}
		else if (!strcmp(argv[i], "--wait"))
		{
			i ++;
			wait = atoi(argv[i]);
			wait *= 1000;
		}
	}

	syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO), "start\n");

	if (do_daemon && daemon(0, 0) == -1)
	{
		syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO), "into daemon failure\n");
	}

	while (1)
	{
		power_on(power_pin);

		if (wait_module_ready(ins, device, buad, apn, wait) == 0)
			goto again;

		start_ppp(device, buad_str, ins, apn, user, passwd, usepeerdns);

	again:
		syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO),"Restart...\n");
		power_off(power_pin);

		sleep(2);
	}

	return 0;
}
