#pragma once

#ifndef _WINDOWS_
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <atomic>
#include <mutex>
#include <string>
#include "XPLMUtilities.h"

#include "units/LLA.h"
#include "data/Sense.h"
#include <data/Aircraft.h>

#define MAC_LENGTH 18

class ResolutionConnection
{
public:
	static unsigned short const K_TCP_PORT = 21218;

	ResolutionConnection(std::string const userMac, std::string const intruderMac, std::string const ip, int const port, Aircraft* userAc);
	~ResolutionConnection();

	std::string const intruderMac;
	std::string const ip; // userIP
	std::string const myMac; //userMAC
	int const port; // listening port
	 
	volatile bool consensusAchieved; 
	volatile Sense currentSense;
	std::mutex lock;
	std::chrono::milliseconds lastAnalyzed;

	LLA userPosition;
	std::chrono::milliseconds userPositionTime;
	LLA userPositionOld;
	std::chrono::milliseconds userPositionOldTime;

	int connectToIntruder(std::string, int);
	SOCKET acceptIncomingIntruder(int);

	DWORD senseSender();
	DWORD senseReceiver();
	int sendSense(Sense);
private:
	static int number_of_connections_; // multiple clients

	std::atomic<bool> running_; 
	std::atomic<bool> threadStopped_;
	std::atomic<bool> connected_;

	// sock_ is used in each method, reset everytime 
	// openSocket is persistent and this represents the client socket in the TCP server (need more clarification on this variable)
	SOCKET sock_, openSocket_;
	struct sockaddr_in myAddr_; // ipv4 -- TCP server
	struct sockaddr_in intruderAddr_; //intruders ipv4 -- TCP client

	void resolveSense();
	void socketDebug(char*, bool);
	void socketCloseWithError(char*, SOCKET);
};

static inline char* const senseToString(Sense s)
{
	switch (s)
	{
	case Sense::UPWARD: return "UPWARD";
	case Sense::DOWNWARD: return "DOWNWARD";
	default: return "UNKNOWN";
	}
}

static inline Sense const stringToSense(char* str)
{
	if (strcmp(str, "UPWARD") == 0) {
		return Sense::UPWARD;
	} else if (strcmp(str, "DOWNWARD") == 0) {
		return Sense::DOWNWARD;
	} else {
		return Sense::UNKNOWN;
	}
}