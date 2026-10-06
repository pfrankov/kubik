#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define inet_ntoa_r(addr, buf, len) inet_ntop(AF_INET, &(addr), (buf), (len))
