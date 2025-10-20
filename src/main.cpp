// g++ -std=c++17 -O2 poll_chat.cpp -o poll_chat
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// helper function to handle error messages
static void die(const char* msg) 
{ 
    std::perror(msg); 
    std::exit(1); 
}

// helper to set a socket in non-blcoking mode
static void set_nonblock(int fd) 
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1) 
    die("fcntl");
}

int main(int argc, char** argv) 
{
    if (argc != 2) { std::cerr << "usage: " << argv[0] << " <port>\n"; return 1; }

    std::signal(SIGPIPE, SIG_IGN); // portable way to avoid SIGPIPE on send()

    addrinfo hints{}; 
    hints.ai_family = AF_UNSPEC; // doesn't matter if IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE; // Give me the host IP

    addrinfo* res = nullptr; // result of getaddrinfo
    if (int rc = getaddrinfo(nullptr, argv[1], &hints, &res); rc != 0) 
    {
        std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n"; return 1;
    }

    int lfd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) // iterates over the linked list of getaddrinfo
    {
        lfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (lfd == -1) 
            continue;
        int yes = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // re-bind the port quickly even if port is in TIME_WAIT state
#ifdef SO_REUSEPORT
        setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
        if (bind(lfd, p->ai_addr, p->ai_addrlen) == 0) // if bind is successful, breaks out of the loop, else close the socket
            break;
        close(lfd); 
        lfd = -1;
    }
    freeaddrinfo(res);
    if (lfd == -1) 
        die("bind"); // if bind was unsuccessful, shows error then terminates
    if (listen(lfd, SOMAXCONN) == -1) // if listen returns an error, shows error then terminates
        die("listen");
    set_nonblock(lfd);

    std::vector<pollfd> pfds; // contains the different pollfd for each client state
    pfds.push_back({lfd, POLLIN, 0}); // index 0 = listening socket, get notified when new connections are ready to be accepted

    std::unordered_map<int, std::string> outbuf; // fd -> pending bytes

    // helper to add new clients to the pfds vec and outbuf map
    auto add_client = [&](int cfd)
    {
        set_nonblock(cfd);
        pfds.push_back({cfd, POLLIN, 0});
        outbuf.try_emplace(cfd);
    };

    // helperto remove clients from the pfds and outbuf map
    auto remove_client = [&](size_t idx) 
    {
        int cfd = pfds[idx].fd;
        close(cfd);
        outbuf.erase(cfd);
        pfds[idx] = pfds.back();
        pfds.pop_back();
    };

    std::cout << "listening on port " << argv[1] << " ...\n";
    char buf[8192];

    while (true) 
    {
        if (poll(pfds.data(), pfds.size(), -1) < 0) 
        {
            if (errno == EINTR) 
                continue;
            die("poll");
        }

        // 1) Accept new clients
        if (pfds[0].revents & POLLIN) 
        {
            while (true) 
            {
                sockaddr_storage ss{}; 
                socklen_t slen = sizeof(ss);
                int cfd = accept(lfd, reinterpret_cast<sockaddr*>(&ss), &slen);
                if (cfd == -1) 
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) 
                        break;
                    std::perror("accept"); 
                    break;
                }
                add_client(cfd);
            }
        }

        // 2) Service clients (iterate backward so removal is O(1))
        for (size_t i = pfds.size(); i > 1; --i) 
        {
            auto& p = pfds[i];
            if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) 
            { 
                remove_client(i); 
                continue; 
            }

            // readable: receive and broadcast
            if (p.revents & POLLIN) 
            {
                while (true) 
                {
                    ssize_t n = recv(p.fd, buf, sizeof(buf), 0);
                    if (n > 0) 
                    {
                        // broadcast to others: append to their out buffers
                        for (size_t j = 1; j < pfds.size(); ++j) 
                        {
                            int ofd = pfds[j].fd;
                            if (ofd == p.fd) 
                                continue;
                            auto& q = outbuf[ofd];
                            q.append(buf, static_cast<size_t>(n));
                            pfds[j].events |= POLLOUT; // ask poll to notify when writable
                        }
                    } 
                    else if (n == 0) 
                    { 
                        remove_client(i); 
                        break; 
                    } // peer closed
                    else 
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) 
                            break;
                        std::perror("recv"); remove_client(i); 
                        break;
                    }
                }
            }

            // writable: flush pending data
            if ((p.revents & POLLOUT) && !outbuf[p.fd].empty()) // if fd is writable and out buffer is not empty
            {
                std::string& q = outbuf[p.fd];
                ssize_t m = send(p.fd, q.data(), q.size(), 0);
                if (m > 0) 
                {
                    q.erase(0, static_cast<size_t>(m));
                    if (q.empty()) 
                        p.events &= ~POLLOUT;
                } 
                else if (m < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) 
                {
                    std::perror("send"); 
                    remove_client(i);
                }
            }
        }
    }
}
