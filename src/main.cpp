#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <print>

int main(int argc, const char** argv)
{
    if (argc != 2) 
    {
        std::println("usage: showip hostname");
        return -1;
    }
    
    int status;
    addrinfo hints{}; // hints for the getaddrinfo() func
    addrinfo* res; // resulting linked-list of the getaddrinfo()
    addrinfo* p; // pointer to iterate over the above linked-list

    hints.ai_family = AF_UNSPEC; // don't care if it is iPv4 or iPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream socket
    hints.ai_flags = AI_PASSIVE; // fill IP for me

    if ((status = getaddrinfo(argv[1], "3490", &hints, &res)) != 0)
    {
        std::println("gai error: {}", gai_strerror(status));
    }

    // Print each addresses of the host given in the command line
    for(p = res; p != nullptr; p = p->ai_next) 
    {
        const void* addr = nullptr;
        const char* ipver = nullptr;

        if (p->ai_family == AF_INET) 
        {
            auto* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            addr = &ipv4->sin_addr;
            ipver = "IPv4";
        } 
        else if (p->ai_family == AF_INET6) 
        {
            auto* ipv6 = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            addr = &ipv6->sin6_addr;
            ipver = "IPv6";
        } 
        else 
        {
            continue; // skip unknown families
        }

        std::array<char, INET6_ADDRSTRLEN> ipstr{};  // big enough for v4 and v6

        // converts the IP address to a string
        if (!inet_ntop(p->ai_family, addr, ipstr.data(), ipstr.size())) 
        {
            // handle error if you want (errno has the reason)
            continue;
        }

        std::println("  {}: {}", ipver, ipstr.data());
    }

    int sockfd; // Socket file descriptor
    if ((sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) == -1)
    {
        std::println("Socket error");
    }

    bind(sockfd, res->ai_addr, res->ai_addrlen);


    freeaddrinfo(res);
}