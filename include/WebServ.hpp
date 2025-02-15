#pragma once

#include "Location.hpp"
#include "Server.hpp"
#include <poll.h>
#include <string>
#include <sys/poll.h>
#include <utility>
#include <vector>

typedef struct sockaddr_in client_info;

class WebServ
{
	public:
	// Methods
	static void startServers(const std::string &configFilePath = "config/webserv.conf");
	static void parseServers(const std::string &configFilePath);
	static void parseCgi(std::ifstream &fin, Location &location);
	static Server parseServerBlock(std::ifstream &fin);
	static std::pair<std::string, std::string> parseListenParams(const std::string &param);
	static void parseErrorPagesBlock(std::ifstream &fin, Server &server);
	static size_t parseClientMaxBodySizeParam(const std::string &param);
	static Location parseLocationBlock(std::ifstream &fin, const std::string &uri);
	static void openSockets();
	static void openSocket(std::map<std::pair<uint32_t, uint16_t>, int> &socketsOpen, Server *server);
	static void run();
	static void pollInit(std::set<int> &listenfds);

	// Attributes
	static std::vector<Server> servers;
	static std::map<int, std::vector<Server *> > serversBySocket;
	static std::vector<struct pollfd> pollfds;

	private:
	static void setDefaultValues();
};
