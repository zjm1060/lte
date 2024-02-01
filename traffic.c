/*
 * traffic.c
 *
 *  Created on: 2024年2月1日
 *      Author: zjm09
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static struct traffic{
	uint64_t tx;
	uint64_t rx;
}traffic;

static uint32_t StringLen(const char *String)
{
	int Result = 0;

	while(String[Result] != '\0' &&
			String[Result] != '\n')
	{
		++Result;
	}

	return Result;
}

static uint64_t StringToU64(const char *String)
{
	uint64_t Result = 0;
	uint64_t TenPower = 1;
	uint32_t StringLenght = StringLen(String);

	for(uint32_t i = StringLenght; i > 0; i--)
	{
		uint64_t Num = String[i - 1] - '0';
		Result += TenPower * Num;
		TenPower = TenPower * 10;
	}

	return Result;
}

void getTrafficDataInSysfs(int ins, struct traffic *data)
{
	char path[512];
	char NumberBuffer[32];
	int File = 0;

	snprintf(path, sizeof(path), "/sys/class/net/ppp%d/statistics/tx_bytes", ins);
	File = open(path, O_RDONLY);
	if(File != -1){
		read(File, NumberBuffer, sizeof(NumberBuffer));
		close(File);

		NumberBuffer[StringLen(NumberBuffer)] = '\0';
		if(data){
			data->tx = StringToU64(NumberBuffer);
		}
	}

	snprintf(path, sizeof(path), "/sys/class/net/ppp%d/statistics/rx_bytes", ins);
	File = open(path, O_RDONLY);
	if(File != -1){
		read(File, NumberBuffer, sizeof(NumberBuffer));
		close(File);

		NumberBuffer[StringLen(NumberBuffer)] = '\0';
		if(data){
			data->rx = StringToU64(NumberBuffer);
		}
	}
}

void saveTrafficData(int ins, const char *suffix, struct traffic *data)
{
	char file_path[512];
	char buffer[32];
	int File;

	snprintf(file_path, sizeof(file_path), "/tmp/LTE%d.%s", ins, suffix);
	snprintf(buffer, sizeof(buffer), "%lu,%lu", data->tx, data->rx);
	File = open(file_path, O_WRONLY | O_TRUNC | O_CREAT);
	write(File, buffer, strlen(buffer));
	close(File);
}

void getSavedTrafficData(int ins, const char *suffix, struct traffic *data)
{
	char file_path[512];
	char buffer[32];
	int File;

	data->tx = 0;
	data->rx = 0;
	snprintf(file_path, sizeof(file_path), "/tmp/LTE%d.%s", ins, suffix);
	File = open(file_path, O_RDONLY);
	if(File != -1){
		read(File, buffer, sizeof(buffer));
		close(File);
		sscanf(buffer, "%lu,%lu", &data->tx, &data->rx);
	}
}

void trafficData(int ins)
{
	struct traffic now;
	struct traffic saved;
	struct traffic data;
	uint64_t diff;

	getTrafficDataInSysfs(ins, &now);
	getSavedTrafficData(ins, "last", &saved);
	getSavedTrafficData(ins, "data", &data);

	if(now.tx < saved.tx){	// new link
		diff = now.tx;
	}else{
		diff = now.tx - saved.tx;
	}
	data.tx += diff;

	if(now.rx < saved.rx){	// new link
		diff = now.rx;
	}else{
		diff = now.rx - saved.rx;
	}
	data.rx += diff;


	saveTrafficData(ins, "last", &now);
	saveTrafficData(ins, "data", &data);
}
