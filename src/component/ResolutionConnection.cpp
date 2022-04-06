#include "ResolutionConnection.h"

int ResolutionConnection::number_of_connections_ = 0;


// start senseReceiver()
static DWORD WINAPI startResolutionReceiver(void* param)
{
	ResolutionConnection* rc = (ResolutionConnection*)param;
	return rc->senseReceiver();
}

// start senseSender()
static DWORD WINAPI startResolutionSender(void* param)
{
	ResolutionConnection* rc = (ResolutionConnection*)param;
	return rc->senseSender();
}

// All args constructor
ResolutionConnection::ResolutionConnection
(
	std::string const mmac, 
	std::string const imac, 
	std::string const ipAddr, 
	int const portNum, 
	Aircraft* userAc
) : myMac(mmac), intruderMac(imac), ip(ipAddr), port(portNum)
{
	userPosition = userAc->positionCurrent;
	userPositionTime = userAc->positionCurrentTime;
	userPositionOld = userAc->positionOld;
	userPositionOldTime = userAc->positionOldTime;
	userAc->lock.unlock();

	running_ = true;
	connected_ = false;
	currentSense = Sense::UNKNOWN;
	consensusAchieved = false;

	LPTHREAD_START_ROUTINE task = strcmp(myMac.c_str(), intruderMac.c_str()) > 0 ? startResolutionSender : startResolutionReceiver;
	DWORD threadID;
	CreateThread(NULL, 0, task, (void*) this, 0, &threadID);
}

/* Cleanup destructor */
ResolutionConnection::~ResolutionConnection()
{
	running_ = false;

	// We need to ensure that the thread using the socket has stopped running before we close the socket so we wait until then
	while (!threadStopped_)
	{
		Sleep(100);
	}

	closesocket(sock_);
}

/*
*	TCP server setup
*	@param port
*	@return the accepted client socket
*/
SOCKET ResolutionConnection::acceptIncomingIntruder(int port) 
{
	// clean up
	memset(&sock_, 0, sizeof sock_);
	memset(&myAddr_, 0, sizeof myAddr_);
	memset(&intruderAddr_, 0, sizeof intruderAddr_);

	// Init TCP listening socket
	sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP + number_of_connections_);

	if (sock_ == INVALID_SOCKET) {
		socketDebug("ResolutionConnection::acceptIncomingIntruder - socket failed to open\n", false);
		return NULL;
	}
	// if sock_ == VALID_SOCKET, then increase number_of_connections_
	number_of_connections_++;

	// set up myAddr to bind the myAddr and port to the socket
	myAddr_.sin_family = AF_INET;
	myAddr_.sin_addr.s_addr = INADDR_ANY;
	myAddr_.sin_port = htons(port); // param port = K_TCP_PORT = 21218

	//test binding
	int bindSuccess = bind(sock_, (struct sockaddr*)&myAddr_, sizeof(myAddr_));

	// if invalid => shut down the server
	if (bindSuccess < 0) {
		char theError[32];
		sprintf(theError, "ResolutionConnection::acceptIncomingIntruder - failed to bind: %d\n", GetLastError());
		XPLMDebugString(theError);
		return NULL;
	}

	SOCKET acceptSocket;

	socklen_t addrLen = sizeof(intruderAddr_);

	// tell winsock that this sock_ socket is fo listening
	if (listen(sock_, 24) == -1) {
		char theError[32];
		sprintf(theError, "ResolutionConnection::acceptIncomingIntruder - failed to listen: %d\n", GetLastError());
		XPLMDebugString(theError);
		return NULL;
	}

	// winsock::accept permits an incoming connection attempt on the listening sock_
	acceptSocket = accept(sock_, (struct sockaddr *)&intruderAddr_, &addrLen);

	// if failed to accept the socket => shutdown  the server
	if (acceptSocket == INVALID_SOCKET) {
		socketCloseWithError("ResolutionConnection::acceptIncomingIntruder - to accept error: %d\n", sock_);
		return NULL;
	}

	// return acceptSocket if successfully accepted by the server
	return acceptSocket;
}

/*
* An implementation of the ResolutionConnection::acceptIncomingIntruder(int port)
* accpetSocket is the representation of the client socket in the TCP Server
*/
DWORD ResolutionConnection::senseReceiver()
{
	char* ack = "ACK";
	
	SOCKET acceptSocket = acceptIncomingIntruder(K_TCP_PORT);

	/*
	  If accpetSpclet is valid => 
		+ toggle connected_ to true
		+ bind global ResolutionConnection::openSocket_ to the acceptSocket (TCP CLIENT)
		+ call ResolutionConnection::resolveSense() to analyze senses
	*/
	if (acceptSocket) {
		connected_ = true;
		openSocket_ = acceptSocket; 
		resolveSense();
	}

	return 0;
}


/*
* TCP Client setup 
* Allows userAC (server) connects to intruderAC (client)
* @param port
* @param ip -- intruder's ipv4
*/
int ResolutionConnection::connectToIntruder(std::string ip, int port)
{
	// init a sockaddr_in structure and clean it
	struct sockaddr_in dest;
	memset(&dest, 0, sizeof dest);

	// Fill in the dest structure to tell winSock what server and what port to connect to 
	dest.sin_family = AF_INET;
	dest.sin_port = htons(port); // using the same port to connect to the tcp server
	dest.sin_addr.s_addr = inet_addr(ip.c_str());

	// reset SOCKET sock_
	memset(&sock_, 0, sizeof sock_);
	
	// init new client socket
	sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// connect the intruder to tcp SERVER socket
	if (connect(sock_, (SOCKADDR*)&dest, sizeof(dest)) == SOCKET_ERROR) {
		socketCloseWithError("ResolutionConnection::connectToIntruder - unable to establish tcp connection - error: %d\n", sock_);
		return -1;
	}
	return 0;
}

/*
* An implementation of the ResolutionConnection::connectToIntruder()
* Allows userAC (server) connects to intruderAC (client)
*/
DWORD ResolutionConnection::senseSender()
{

	/*
	* If ResolutionConnection::connectToIntruder() is invalid => shutdown
	* Else =>
	*	+ toggle connected_ to true
	*	+ bind global ResolutionConnection::openSocket_ to the sock_
	*	+ call ResolutionConnection::resolveSense() to analyze senses
	*/
	if (connectToIntruder(ip, port) < 0) {
		std::string dbgstring = "ResolutionConnection::senseSender - failed to establish connection to " + ip + "\n";
		XPLMDebugString(dbgstring.c_str());
	} else {
		connected_ = true;
		openSocket_ = sock_;
		resolveSense();
	}
	return 0;
}


/*
* A function to analyze and resolve Senses
* Interactions between TCP Servers and TCP Clients
*/
void ResolutionConnection::resolveSense()
{
	char msg[256]; // buffer
	static char* ack = "ACK"; // ack = acknowledgement packet is sent back to a client after a server received a message to let the client know the server got the package

	while (running_) // running_ is set true in constructor and set false when it's closed
	{
		// recv() wait for messages from the SERVER, if (message) then write message into "msg" buffer
		if (recv(openSocket_, msg, 255, 0) < 0) { // if fail
			socketCloseWithError("ResolutionConnection::resolveSense - Failed to receive: %d\n", openSocket_);
		} else {

			// lock before any manipulation
			lock.lock();

			if (strcmp(msg, ack) == 0) { // mgs = ack means the packet is successfully transferred
				XPLMDebugString("ResolutionConnection::resolveSense() - ack received! Setting consensus to true...\n");
				consensusAchieved = true; 
				lock.unlock();
			} else { // mgs != ack
				// received a sense which is not UNKNOWN, store in msg buffer
				
				// if the current sense is unknown
				if (currentSense == Sense::UNKNOWN) {

					// assign new sense in msg buffer to currentSense
					currentSense = stringToSense(msg);

					// send to client the ack packet confirmation
					if (send(openSocket_, ack, strlen(ack) + 1, 0) == SOCKET_ERROR) { //if fail
						lock.unlock();
						socketCloseWithError("ResolutionConnection::resolveSense() - failed to send ack after receiving intruder sense with user_sense unknown\n", openSocket_);
					} else { // if succeed that means the client got the ack packet
						consensusAchieved = true;
						lock.unlock();
						XPLMDebugString("ResolutionConnection::resolveSense() - achieved consensus in case where intruder sent sense first\n");
					}

				// if the current sense is not unknown that means there is already an intruder
				} else {
					XPLMDebugString("ResolutionConnection::resolveSense - edge case entered - current sense is not unknown! received intruder sense\n");
					Sense senseCurrent = currentSense;
					lock.unlock();

					char debugBuf[256];
					char debuggingsense[512];
					 
					// start comparing myMac and intrMac
					snprintf(debuggingsense, 512, "ResolutionConnection::resolveSense Compare \nmyMac: %s\ninteruderMac: %s\n", myMac.c_str(), intruderMac.c_str());
					XPLMDebugString(debuggingsense);

					// if (myMac > intruMac) then send sense (SENDER)
					if (strcmp(myMac.c_str(), intruderMac.c_str()) > 0) {
						snprintf(debugBuf, 256, "ResolutionConnection::resolveSense - edge case with my_mac > intr_mac; sending sense: %s\n", senseToString(senseCurrent));
						XPLMDebugString(debugBuf);

						// send sense 
						sendSense(senseCurrent);

						// wait for a packet back from client
						if (recv(openSocket_, msg, 255, 0) < 0) {
							socketCloseWithError("ResolutionConnection::resolveSense - edge case - failed to receive: %d\n", openSocket_);
						} else { // if recv is successful
							if (strcmp(msg, ack) == 0) { // server got the message
								lock.lock();
								consensusAchieved = true;
								lock.unlock();


								XPLMDebugString("ResolutionConnection::resolveSense - achieved consensus for edge case with user_mac > intr_mac\n");

							} else {
								socketCloseWithError("ResolutionConnection::resolveSense - Failed to receive ack in edge case with user_mac > intr_mac: %d\n", openSocket_);
							}
						}
					} 
					else // else if (myMac < intrMac) then wait to receive sense (RECEIVER) 
					{
						snprintf(debugBuf, 256, "ResolutionConnection::resolveSense - edge case with my mac < intr_mac with sense: %s; waiting to receive sense.\n", senseToString(senseCurrent));
						XPLMDebugString(debugBuf);

						
						if (recv(openSocket_, msg, 255, 0) < 0) {
							socketCloseWithError("ResolutionConnection::resolveSense - failed to receive sense from intr in edge case with user_mac < intr_mac: %d\n", openSocket_);

						} 
						else 
						{ // if recv from openSocket is successful 
							Sense senseFromIntruder = stringToSense(msg); // assign new sense from Intrudeer because we're going to be a receiver now
							debugBuf[0] = '\0'; 

							snprintf(debugBuf, 256, "ResolutionConnection::resolveSense - received sense %s from intruder.\n", msg);
							XPLMDebugString(debugBuf);

							// send ACK to client
							if (send(openSocket_, ack, strlen(ack) + 1, 0) == SOCKET_ERROR) {
								socketCloseWithError("ResolutionConnection::resolveSense - Failed to send ack in edge case with user_mac < intr_mac\n", openSocket_);
							} 
							else // if client got ACK then switch currentSense to the opposite sense
							{
								lock.lock();
								consensusAchieved = true;
								currentSense = senseutil::oppositeFromSense(senseFromIntruder);
								lock.unlock();

								XPLMDebugString("ResolutionConnection::resolveSense - Achieved consensus in edge case with user_mac < intr_mac\n");
							}
						}
					}
				}
			}
		}
	}
	threadStopped_ = true;
}

/*
* Sends senses to intruder(client)
* @param Sense::currSense
*/
int ResolutionConnection::sendSense(Sense currSense)
{
	if (currSense == Sense::UPWARD) {
		currSense = Sense::DOWNWARD;
	} else if (currSense == Sense::DOWNWARD) {
		currSense = Sense::UPWARD;
	} else {
		// Sending a sense of unknown or maintained will cause undefined behavior
		XPLMDebugString("ResolutionConnection::sendSense - Sense is UNKNOWN or MAINTAINED\n");
	}

	// if userAC and intrAC connected 
	if (connected_) {
		char* msg = senseToString(currSense); // store currSense in msg

		// send to openSocket (client) the sense (msg)
		if (send(openSocket_, msg, strlen(msg) + 1, 0) == SOCKET_ERROR) {
			socketCloseWithError("ResolutionConnection::resolveSense - ack failed: %d\n", openSocket_);
			return -1;
		} else {
			return 0; // success
		}
	} else {
		XPLMDebugString("ResolutionConnection::sendSense - attempting to send on an unconnected socket\n");
		return -2;
	}
}

void ResolutionConnection::socketDebug(char* errMsg, bool closeSocket)
{
	running_ = false;

	char debugBuf[128];
	snprintf(debugBuf, 128, "ResolutionConnection::socketDebug - %s\n", errMsg);
	XPLMDebugString(debugBuf);
	if (closeSocket)
		closesocket(sock_);
}

void ResolutionConnection::socketCloseWithError(char* errMsg, SOCKET openSock)
{
	running_ = false;

	char debugBuf[128];
	snprintf(debugBuf, 128, errMsg, GetLastError());
	XPLMDebugString(debugBuf);
	closesocket(openSocket_);
}