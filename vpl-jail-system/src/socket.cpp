/**
 * package:		Part of vpl-jail-system
 * copyright:	Copyright (C) 2014 Juan Carlos Rodríguez-del-Pino
 * license:		GNU/GPL, see LICENSE.txt or http://www.gnu.org/licenses/gpl-3.0.html
 **/
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include "socket.h"
#include "util.h"
#include "jail_limits.h"


SSLBase* SSLBase::singlenton=NULL;

vplregex Socket::regRequestLine("^([^ ]+) ([^ ]+) ([^ ]+)$");
vplregex Socket::regHeader("^[ \t]*([^ \t:]+):[ \t]*(.*)$");
vplregex Socket::regURL("^([a-zA-Z]+)?(:\\/\\/[^\\/]+)?([^?]*)[?]?(.*)?$");
vplregex Socket::regCookie("([^=]+)=([^;]+)(; )?");

/**
 * Process the http request line
 * parse method, URL and version
 * 
 * @param line string to process
 */
void Socket::parseRequestLine(const string &line){
	syslog(LOG_DEBUG,"Request line :\"%s\"", line.c_str());
	method = "";
	URL = "";
	version = "";
	protocol = "";
	URLPath = "";
	vplregmatch match(5);
	bool nomatch = ! regRequestLine.search(line, match);
	if (nomatch) {
		throw HttpException(badRequestCode, "Erroneous request line", line);
	}
	method = match[1];
	syslog(LOG_DEBUG,"method :\"%s\"", method.c_str());
	URL = match[2];
	syslog(LOG_DEBUG,"URL :\"%s\"", URL.c_str());
	version = match[3];
	syslog(LOG_DEBUG,"version :\"%s\"",version.c_str());

	nomatch = ! regURL.search(URL, match);
	if (nomatch) {
		throw HttpException(badRequestCode, "Erroneous URL", URL);
	}
	protocol = match[1];
	URLPath = match[3];
	queryString = match[4];
}
string Socket::getHeader(string name){
	name=Util::toUppercase(name);
	if(headers.find(name) != headers.end())
		return headers[name];
	return "";
}

/**
 * Parses cookie param and save it in cookies map
 * 
 * @param rawcookies cookie header value to process
 */
void Socket::parseCookies(const string &value){
	string rawcookies = value;
	syslog(LOG_INFO,"Cookie :\"%s\"", rawcookies.c_str());
	vplregmatch match(3);
	size_t nmatchs = 0;
	while (regCookie.search(rawcookies, match)) {
		nmatchs++;
		cookies[match[1]] = match[2];
		rawcookies = rawcookies.substr(match[0].length());
	}
	if (cookies.size() != nmatchs) {
		syslog(LOG_DEBUG, "Bad cookiees (repeated): %s", value.c_str());
		throw HttpException(badRequestCode, "Erroneous cookie (repeated)", value);
	}
}

/**
 * Process one header
 * Parse header and save it in header map
 * 
 * @param line header to process
 */
void Socket::parseHeader(const string &line){
	syslog(LOG_INFO,"Header :\"%s\"", line.c_str());
	vplregmatch match(3);
	bool nomatch = !regHeader.search(line, match);
	if (nomatch) {
		syslog(LOG_DEBUG, "No match header: %s", line.c_str());
		throw HttpException(badRequestCode, "Erroneous header", line);
	}
	string field = Util::toUppercase(match[1]);
	string value = match[2];
	headers[field] = value;
	if (field == "COOKIE") {
		parseCookies(value);
	}
}
/**
 * Process http request headers (until CRLFCRLF)
 * Check request line and parse headers
 * 
 * @param input lines that form the request line and headers
 */
void Socket::processHeaders(const string &input){
	string line;
	size_t offset = 0;
	syslog(LOG_DEBUG,"processHeaders: %s",input.c_str());
	do {
		line = Util::getLine(input, offset);
	} while(line.size() == 0 && offset < input.size());
	parseRequestLine(line);
	while((line=Util::getLine(input,offset)).size()){
		parseHeader(line);
	}
}

Socket::Socket(int socket){
	struct sockaddr_in jail_client;
	socklen_t snlen = sizeof(jail_client);
	if(getpeername(socket, (struct sockaddr*)&jail_client, &snlen)){
		syslog(LOG_ERR, "getpeername fail %m");
	}else{
		this->clientip = jail_client.sin_addr.s_addr;
		unsigned char *CIP = (unsigned char *)&(this->clientip);
		syslog(LOG_INFO, "Client: %d.%d.%d.%d", (int)CIP[0], (int)CIP[1], (int)CIP[2], (int)CIP[3]);
	}
	this->maxDataSize = Configuration::getConfiguration()->getRequestMaxSize();
	this->socket = socket;
	this->closed = false;
}
Socket::~Socket(){
	close();
}

/**
 * Close socket
 */
void Socket::close(){
	if( ! closed)  {
		shutdown(socket,SHUT_RDWR);
		closed = true;
	}
}

/**
 * Return cookie value or empty string if not found
 */
string Socket::getCookie(string cookie) {
	if (cookies.find(cookie) != cookies.end()) {
		return cookies[cookie];
	}
	return "";
}

/**
 * Read http headers until end of headers found
 */
void Socket::readHeaders(){
	if (header.empty()) {
		receive(); //First receive => read and parse http headers
	}
}

string Socket::receive(int sizeToReceive){ //=0 async read
	if (closed) {
		string ret = readBuffer;
		readBuffer = "";
		if (ret.size()) {
			syslog(LOG_INFO,"Received %lu", (long unsigned int) ret.size());
		}
		return ret;
	}
	if (header.empty()) {
		sizeToReceive = JAIL_HEADERS_SIZE_LIMIT;
	}
	if (sizeToReceive > 0) {
		syslog(LOG_INFO,"Receiving until %d bytes", sizeToReceive);
	}
	//If already read, return data
	if( sizeToReceive > 0 && (int) readBuffer.size() >= sizeToReceive
			&& ! header.empty()){
		string ret = readBuffer.substr(0, sizeToReceive);
		readBuffer.erase(0, sizeToReceive);
		return ret;
	}
	struct pollfd devices[1];
	devices[0].fd = socket;
	devices[0].events = POLLIN;
	const int MAX = JAIL_NET_BUFFER_SIZE;
	char buf[MAX];
	const int wait = 10; // 10 milisec
	const int bad = POLLERR|POLLNVAL;
	time_t timeLimit = time(NULL)+JAIL_SOCKET_TIMEOUT;
	time_t fullTimeLimit = time(NULL)+JAIL_SOCKET_REQUESTTIMEOUT;
	while (true) {
		int res = poll(devices,1,wait);
		if(res == -1) {
			syslog(LOG_INFO,"poll fail reading %m");
			throw HttpException(internalServerErrorCode
					,"Error poll reading data"); //Error
		}
		time_t currentTime = time(NULL);
		if (currentTime > timeLimit || currentTime > fullTimeLimit) {
			if(sizeToReceive == 0){
				syslog(LOG_DEBUG,"Socket read timeout, closed connection?");
				return "";
			}else {
				throw HttpException(requestTimeoutCode, "Socket read timeout");
			}
		}
		if (res == 0 && sizeToReceive == 0) break; //Nothing to read
		if (res == 0) continue; //Nothing to do
		syslog(LOG_DEBUG,"poll return: %d", res);
		if (devices[0].revents & POLLIN) { //Read from net
			int sizeRead = netRead(buf, MAX);
			if(sizeRead > 0) {
				string sread(buf, sizeRead);
				if( (int) sread.size() != sizeRead) {
					syslog(LOG_ERR,"read net decode error");
				}
				readBuffer += sread;
				if (header.empty()) {
					size_t pos;
					if ((pos = readBuffer.find("\r\n\r\n")) != string::npos) {
						header = readBuffer.substr(0, pos + 4);
						syslog(LOG_INFO, "Received header %lu",(long unsigned int)pos+4);
						processHeaders(header);
						readBuffer.erase(0,pos+4);
						return ""; //End of process
					} else if (readBuffer.size() > JAIL_HEADERS_SIZE_LIMIT) {
						throw HttpException(requestEntityTooLargeCode, "Http headers too large");
					}
				}else if(sizeToReceive == 0 || (int) readBuffer.size() >= sizeToReceive){ //Receive complete
					break;
				}
				timeLimit = currentTime + JAIL_SOCKET_TIMEOUT; //Reset timeout
			} else if (sizeRead < 0) {
				throw HttpException(badRequestCode, "Error reading data");
			} else {
				syslog(LOG_INFO, "sizeRead==0");
				closed = true;
				break;
			}
		}
		if (devices[0].revents & POLLHUP) { //socket close
			closed = true;
			syslog(LOG_INFO, "POLLHUP");
			break;
		}
		if (devices[0].revents & bad) {
			throw HttpException(internalServerErrorCode, "Error reading data");
		}
	}
	string ret = readBuffer;
	readBuffer = "";
	if (ret.size()) {
		syslog(LOG_INFO, "Received %lu", (long unsigned int) ret.size());
	}
	return ret;
}

void Socket::send(const string &data, bool async){
	if(closed) return;
	writeBuffer += data;
	size_t size=writeBuffer.size();
	const unsigned char *s=(const unsigned char *)writeBuffer.data();
	syslog(LOG_INFO,"Sending %lu to fd %u",(long unsigned int)size,socket);
	size_t offset=0;
	struct pollfd devices[1];
	devices[0].fd=socket;
	devices[0].events=POLLOUT;
	const int wait=10; // 10 milisec
	const int bad=POLLERR|POLLHUP|POLLNVAL;
	time_t timeLimit=time(NULL)+JAIL_SOCKET_TIMEOUT;
	time_t fullTimeLimit=time(NULL)+JAIL_SOCKET_REQUESTTIMEOUT;
	while(true){
		int res=poll(devices,1,wait);
		if(res==-1) {
			throw HttpException(internalServerErrorCode
					,"Error poll writing data"); //Error
		}
		if(res==0 && async) break; //async write Nothing to do
		if(res==0) continue; //Nothing to do
		time_t currentTime=time(NULL);
		if(currentTime>timeLimit || currentTime>fullTimeLimit){
			syslog(LOG_ERR,"Socket write timeout");
			throw requestTimeoutCode;
		}
		if(devices[0].revents & POLLOUT){ //Write to net
			size_t toWrite=JAIL_NET_BUFFER_SIZE;
			if(toWrite>size-offset)
				toWrite=size-offset;
			int sizeWritten=netWrite(s+offset,toWrite);
			if(sizeWritten <0) {
				throw HttpException(internalServerErrorCode
						,"Socket write data error");
			}if(sizeWritten == 0){
				closed=true;
				break;
			}
			else{
				offset += sizeWritten;
				timeLimit=currentTime+JAIL_SOCKET_TIMEOUT; //Reset timeout
			}
		}
		if(devices[0].revents & POLLHUP){ //socket close
			closed=true;
			syslog(LOG_INFO,"POLLHUP");
			break;
		}
		if(devices[0].revents & bad) {
			syslog(LOG_ERR,"Error writing http data %m");
			throw HttpException(internalServerErrorCode
					,"Socket write data error");
		}
		if(offset>=size) break;
	}
	writeBuffer.erase(0,offset);
	syslog(LOG_INFO,"Send %lu",(long unsigned int)offset);
}

bool Socket::wait(const int msec){
	if(readBuffer.size()>0) return false;
	struct pollfd devices[1];
	devices[0].fd=socket;
	devices[0].events=POLLIN;
	if(writeBuffer.size()>0)
		devices[0].events|=POLLOUT;
	int res=poll(devices,1,msec);
	//write pending?
	if(writeBuffer.size()>0 && res == 1 && (devices[0].revents & POLLOUT)){
		send("");
		return false;
	}
	return res==0;
}

ssize_t Socket::netWrite(const void *b, size_t s){
	return write(socket,b,s);
}

ssize_t Socket::netRead(void *b, size_t s){
	return read(socket,b,s);
}


