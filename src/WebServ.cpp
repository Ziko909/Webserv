#include "WebServ.hpp"
#include "ConfigChecker.hpp"
#include "Exception.hpp"
#include "Http.hpp"
#include "Location.hpp"
#include "Utils.hpp"
#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <set>
#include <string>
#include <sys/fcntl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

std::vector<Server> WebServ::servers;
std::map<int, std::vector<Server *> > WebServ::serversBySocket;
std::vector<struct pollfd> WebServ::pollfds;

void WebServ::startServers(const std::string &configFilePath)
{
	ConfigChecker configChecker(configFilePath);
	parseServers(configFilePath);
	setDefaultValues();
	run();
}

void WebServ::parseServers(const std::string &configFilePath)
{
	std::ifstream fin(configFilePath);
	if (not fin.is_open())
		throw FileException("Failed to open config file: " + configFilePath);
	
	std::string line;
	while (std::getline(fin, line))
	{
		std::replace(line.begin(), line.end(), '\t', ' ');
		line = Utils::Trim(line);
		if (line.empty() or line[0] == '#')
			continue;
		
		servers.push_back(parseServerBlock(fin));
	}
}

Server WebServ::parseServerBlock(std::ifstream &fin)
{
	Server server;
	std::string line;
	std::set<std::pair<uint32_t, uint16_t> > listenDirectives;

	while (std::getline(fin, line))
	{
		std::replace(line.begin(), line.end(), '\t', ' ');
		line = Utils::Trim(line);
		if (line.empty() or line[0] == '#')
			continue;
		
		if (line == "}")
			break;
		
		std::vector<std::string> tokens = Utils::Split(line, ' ');
		if (tokens.front() == "listen")
		{
			for (size_t i = 1; i < tokens.size(); ++i)
			{	
				std::pair<std::string, std::string> listenParams = parseListenParams(tokens[i]);
				server.addListen(listenParams.first, listenParams.second, listenDirectives);
			}
		}
		else if (tokens.front() == "server_name")
		{
			for (size_t i = 1; i < tokens.size(); ++i)
				server.addServerName(tokens[i]);
		}
		else if (tokens.front() == "error_pages")
		{
			parseErrorPagesBlock( fin, server);
		}
		else if (tokens.front() == "client_max_body_size")
		{
			server.clientMaxBodySize = parseClientMaxBodySizeParam(tokens[1]);
		}
		else if (tokens.front() == "location")
		{
			server.addLocation(parseLocationBlock(fin, tokens[1]));
		}
	}
	return server;
}

void WebServ::parseCgi(std::ifstream &fin, Location &location)
{
	std::string line;

	while (std::getline(fin, line))
	{
		std::replace(line.begin(), line.end(), '\t', ' ');
		line = Utils::Trim(line);
		if (line.empty() or line[0] == '#')
			continue;
		
		if (line == "}")
			break;

		std::vector<std::string> tokens = Utils::Split(line, ' ');
		location.addCgi(tokens[0], tokens[1]);
	}
}

std::pair<std::string, std::string> WebServ::parseListenParams(const std::string &param)
{
	std::pair<std::string, std::string> listenParams;
	std::vector<std::string> tokens = Utils::Split(param, ':');
	if (tokens.size() == 1 and not ConfigChecker::validatePortNumber(tokens[0]))
	{
		listenParams.first = tokens[0];
		listenParams.second = "8080";
	}
	else if (tokens.size() == 2)
	{
		listenParams.first = tokens[0];
		listenParams.second = tokens[1];
	}
	else
	{
		listenParams.first = "0.0.0.0";
		listenParams.second = tokens[0];
	}
	return listenParams;
}

void WebServ::parseErrorPagesBlock(std::ifstream &fin, Server &server)
{
	std::string line;
	while (std::getline(fin, line))
	{
		std::replace(line.begin(), line.end(), '\t', ' ');
		line = Utils::Trim(line);
		if (line.empty() or line[0] == '#')
			continue;
		
		if (line == "}")
			break;
		
		std::vector<std::string> tokens = Utils::Split(line, ' ');
		for (size_t i = 0; i < tokens.size() - 1; ++i)
			server.addErrorPage(tokens[i], tokens.back());
	}
}

size_t WebServ::parseClientMaxBodySizeParam(const std::string &param)
{
	const std::string SIZE_SUFFIXES = "kKmM";
	size_t size = 0;
	size_t digitsLength = param.find_first_not_of("0123456789");
	if (digitsLength == std::string::npos)
		size = std::stoul(param);
	else
	{
		size = std::stoul(param.substr(0, digitsLength));
		char suffix = param[digitsLength];
		if (suffix == 'k' or suffix == 'K')
			size = size * 1024;
		else if (suffix == 'm' or suffix == 'M')
			size = size * 1024 * 1024;
	}
	return size;
}

Location WebServ::parseLocationBlock(std::ifstream &fin, const std::string &uri)
{
	Location location;
	std::string line;

	location.uri = uri;
	while (std::getline(fin, line))
	{
		std::replace(line.begin(), line.end(), '\t', ' ');
		line = Utils::Trim(line);
		if (line.empty() or line[0] == '#')
			continue;
		
		if (line == "}")
			break;
		
		std::vector<std::string> tokens = Utils::Split(line, ' ');
		if (tokens.front() == "root")
		{
			location.root = tokens[1];
		}
		else if (tokens.front() == "index")
		{
			for (size_t i = 1; i < tokens.size(); ++i)
				location.addIndex(tokens[i]);
		}
		else if (tokens.front() == "autoindex")
		{
			location.autoIndex = tokens[1] == "on";
		}
		else if (tokens.front() == "upload")
		{
			location.upload = true;
			location.uploadPath = tokens[1];
		}
		else if (tokens.front() == "redirect")
		{
			location.createRedirection(tokens[2], tokens[1]);
		}
		else if (tokens.front() == "allowed_methods")
		{
			for (size_t i = 1; i < tokens.size(); ++i)
				location.addAllowedMethod(tokens[i]);
		}
		else if (tokens.front() == "cgi")
		{
			parseCgi(fin, location);
		}
	}
	return location;
}

void WebServ::openSocket(std::map<std::pair<uint32_t, uint16_t>, int> &socketsOpen, Server *server)
{
	for (size_t i = 0; i < server->listen.size(); ++i)
	{
		struct addrinfo *result = NULL;
		struct addrinfo hints = {};
		int status = 0;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;	
		status = getaddrinfo(
			server->listen[i].first.c_str(),
			server->listen[i].second.c_str(),
			&hints,
			&result
		);
		if (status == -1)
		{
			std::cerr << "getaddrinfo error: " << gai_strerror(status) << std::endl;
			exit(1);
		}

		std::pair<uint32_t, uint16_t> addr = std::make_pair(
			(reinterpret_cast<struct sockaddr_in *>(result->ai_addr))->sin_addr.s_addr,
			(reinterpret_cast<struct sockaddr_in *>(result->ai_addr))->sin_port
		);

		if (socketsOpen.find(addr) != socketsOpen.end())
		{
			int socketFd = socketsOpen[addr];

			serversBySocket[socketFd].push_back(server);
		}
		else
		{
			int socketFd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
			if (socketFd == -1)
			{
				std::cerr << "socket error: " << strerror(errno) << std::endl;
				exit(1);
			}

			int optval = 1;
			if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
			{
				std::cerr << "setsockopt error: " << strerror(errno) << std::endl;
				exit(1);
			}

			if (fcntl(socketFd, F_SETFL, O_NONBLOCK) == -1)
			{
				std::cerr << "fcntl error: " << strerror(errno) << std::endl;
				exit(1);
			}


			if (bind(socketFd, result->ai_addr, result->ai_addrlen) == -1)
			{
				std::cerr << "bind error: " << strerror(errno) << std::endl;
				exit(1);
			}

			if (listen(socketFd, SOMAXCONN) == -1)
			{
				std::cerr << "listen error: " << strerror(errno) << std::endl;
				exit(1);
			}

			socketsOpen[addr] = socketFd;
			serversBySocket[socketFd].push_back(server);
		}
		freeaddrinfo(result);
	}
}

void WebServ::openSockets()
{
	std::map<std::pair<uint32_t, uint16_t>, int> socketsOpen;

	for (size_t i = 0; i < servers.size(); ++i)
		openSocket(socketsOpen, &servers[i]);
}

void WebServ::pollInit(std::set<int> &listenfds)
{
	openSockets();
	for (std::map<int, std::vector<Server *> >::iterator it = serversBySocket.begin(); it != serversBySocket.end(); ++it)
	{
		int socketFd = it->first;
		listenfds.insert(socketFd);
		pollfds.push_back((struct pollfd) {
			.fd = socketFd,
			.events = POLLIN | POLLOUT,
			.revents = 0
		});
	}
}

void WebServ::run()
{
	std::set<int> listenfds;
	std::map<int, Http *> httpByFd;
	std::map<int, client_info>	clientByFd;

	pollInit(listenfds);
	while (true)
	{
		int status = poll(pollfds.data(), pollfds.size(), -1);
		if (status == -1)
		{
			std::cerr << "poll error: " << strerror(errno) << std::endl;
			exit(1);
		}
		for (size_t i = 0, j = 0; i < pollfds.size() and j < static_cast<size_t>(status); ++i)
		{
			if (pollfds[i].revents == 0)
				continue;
			++j;
			if (pollfds[i].revents & POLLIN)
			{
				if (listenfds.find(pollfds[i].fd) != listenfds.end())
				{
					client_info	client;
					socklen_t	len;
					
					int clientFd = accept(pollfds[i].fd, reinterpret_cast<struct sockaddr *>(&client), &len);
					if (clientFd == -1)
					{
						std::cerr << "accept error: " << strerror(errno) << std::endl;
						exit(1);
					}

					if (fcntl(clientFd, F_SETFL, O_NONBLOCK) == -1)
					{
						std::cerr << "fcntl error: " << strerror(errno) << std::endl;
						exit(1);
					}

					clientByFd[clientFd] = client;
					serversBySocket[clientFd] = serversBySocket[pollfds[i].fd];
					httpByFd[clientFd] = new Http;
					pollfds.push_back((struct pollfd) {
						.fd = clientFd,
						.events = POLLIN | POLLOUT,
						.revents = 0
					});
				}
				else
				{
					httpByFd[pollfds[i].fd]->readRequest(pollfds[i].fd, clientByFd[pollfds[i].fd]);
				}
			}
			if (pollfds[i].revents & POLLOUT)
			{
				if (not httpByFd[pollfds[i].fd]->sendResponse(pollfds[i].fd))
				{
					delete httpByFd[pollfds[i].fd];
					httpByFd.erase(pollfds[i].fd);
					serversBySocket.erase(pollfds[i].fd);
					close(pollfds[i].fd);
					pollfds.erase(pollfds.begin() + static_cast<ssize_t>(i));
					--i;
				}
			}
		}
	}
}

void WebServ::setDefaultValues()
{
	for (size_t i = 0; i < servers.size(); ++i)
	{
		Server &server = servers[i];

		if (server.listen.empty())
			server.listen.push_back( std::make_pair("0.0.0.0", "8080") );

		if (server.serverNames.empty())
			server.serverNames.push_back(server.listen.front().first);

		if (server.locations.empty())
		{
			server.locations.push_back(Location());
		}

		for (size_t j = 0; j < server.locations.size(); ++j)
		{
			if (server.locations[j].root.empty())
				server.locations[j].root = "./html";
			if (server.locations[j].allowedMethods.empty())
				server.locations[j].allowedMethods.insert("GET");
		}
	}
}
