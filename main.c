/*
 * main.c
 *
 *  Created on: 2020��9��18��
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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

static void usage(char *self)
{
	fprintf(stderr, "%s [options]\n", basename(self));
	fprintf(stderr, "options:\n", basename(self));
	fprintf(stderr, "\t--device dev\tSpecify the port used for dialing\n\t\t\tdefault is /dev/ttyS0\n");
	fprintf(stderr, "\t--buad buad\tSpecify the port baud rate, default is 115200\n");
	fprintf(stderr, "\t--apn xxx\tSpecify the APN, default is cmnet\n");
	fprintf(stderr, "\t--user xxx\tSpecify the user\n");
	fprintf(stderr, "\t--passwd xxx\tSpecify the password\n");
	fprintf(stderr, "\t--power-ctrl n\tSet power control pin\n\t\t\tif not set than will not control the lte module power\n");
	fprintf(stderr, "\t--unit n\tSets the ppp unit number for outbound connections\n\t\t\tdefault is 0\n");
	fprintf(stderr, "\t--no-daemon\tForeground execution\n");
	fprintf(stderr, "\t--help\tShow this message\n");
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
	write(fd, cmd, strlen(cmd));
	if (device_read(fd, response, size, wait))
	{
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

int wait_module_ready(const char *device, int buad, const char *apn, int wait)
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
	char buffer[32];
	while (access(device, 0))
	{ // wait module ready
		chk_time(wait);
	}

	if ((fd = open_device(device, buad)) == -1)
	{
		return 0;
	}

	while (atCommand(fd, "AT\r", buffer, 32, 100))
	{
		chk_time(wait);
	}

	atCommand(fd, "ATE0\r", buffer, 32, 100);

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
				res = 1;
				break;
			}
			else if (strstr(buffer, "+CREG: 0,5") != NULL)
			{
				//Registered (roaming)
				res = 1;
				break;
			}
		}

		chk_time(wait);
	}

out:
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
	}

	return "0.0.0.0";
}

void start_ppp(const char *device, const char *buad, int uint, const char *apn, const char *user, const char *passwd, int force_route)
{
	// pppd $device $buad noauth nodetach nocrtscts noipdefault usepeerdns defaultroute \
	// user "$user" password "$passwd" connect "chat -v -E -f /etc/ppp/lte_connect.script"
	char unit[2];
	const char *argv[20];

	unit[0] = '0'+uint;
	unit[1] = 0;

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
	argv[15] = "unit";
	argv[16] = unit;
	argv[17] = NULL;

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
			char ifname[IFNAMSIZ];
			sleep(3);
			snprintf(ifname, sizeof(ifname), "ppp%d", uint);
			char *ip =  getip(ifname);
//			printf("ip:%s\n",ip);
			if(!strcmp("0.0.0.0",ip)){
				fprintf(stderr, "%s is down\n", ifname);
				kill(pid, SIGTERM);
			}
		}

		fprintf(stderr, "pppd is exit,restart it\n");
		signal(SIGCHLD, SIG_IGN);
	}
}

#define power_on(a) power(a, 1)
#define power_off(a) power(a, 0)

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
	const char *device = "/dev/ttyUSB2";
	const char *apn = "cmnet";
	const char *user = "";
	const char *passwd = "";
	int buad = B115200;
	char *buad_str = "115200";
	int ins = 0;
	int force_route = 0;

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
		else if (!strcmp(argv[i], "--power-ctrl"))
		{
			i++;
			power_pin = atoi(argv[i]);
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
	}

	if (do_daemon && daemon(0, 0) == -1)
	{
		fprintf(stderr, "into daemon failure\n");
	}

	while (1)
	{
		power_on(power_pin);

		if (wait_module_ready(device, buad, apn, 50 * 1000) == 0)
			goto again;

		start_ppp(device, buad_str, ins, apn, user, passwd, force_route);

	again:
		power_off(power_pin);

		sleep(2);
	}

	return 0;
}
