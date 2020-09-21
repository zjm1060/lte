CC=arm-none-linux-gnueabi-gcc

lte_connect: 
	arm-none-linux-gnueabi-gcc -mcpu=arm926ej-s -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -std=gnu11 -o lte_connect main.c -lc