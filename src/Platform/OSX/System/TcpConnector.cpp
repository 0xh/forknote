// Copyright (c) 2012-2015, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "TcpConnector.h"
#include <cassert>

#include <fcntl.h>
#include <netdb.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <System/InterruptedException.h>
#include <System/Ipv4Address.h>
#include "Dispatcher.h"
#include "TcpConnection.h"

namespace System {

namespace {

struct ConnectorContext : public Dispatcher::OperationContext {
  int connection;
};

}

TcpConnector::TcpConnector() : dispatcher(nullptr) {
}

TcpConnector::TcpConnector(Dispatcher& dispatcher) : dispatcher(&dispatcher), stopped(false), context(nullptr) {
}

TcpConnector::TcpConnector(TcpConnector&& other) : dispatcher(other.dispatcher) {
  if (other.dispatcher != nullptr) {
    assert(other.context == nullptr);
    stopped = other.stopped;
    context = nullptr;
    other.dispatcher = nullptr;
  }
}

TcpConnector::~TcpConnector() {
  assert(dispatcher == nullptr || context == nullptr);
}

TcpConnector& TcpConnector::operator=(TcpConnector&& other) {
  dispatcher = other.dispatcher;
  if (other.dispatcher != nullptr) {
    assert(other.context == nullptr);
    stopped = other.stopped;
    context = nullptr;
    other.dispatcher = nullptr;
  }

  return *this;
}

void TcpConnector::start() {
  assert(dispatcher != nullptr);
  assert(stopped);
  stopped = false;
}

void TcpConnector::stop() {
  assert(dispatcher != nullptr);
  assert(!stopped);
  if (context != nullptr) {
    ConnectorContext* connectorContext = static_cast<ConnectorContext*>(context);
    if (!connectorContext->interrupted) {
      if (close(connectorContext->connection) == -1) {
        throw std::runtime_error("TcpListener::stop, close failed, errno=" + std::to_string(errno));
      }

      dispatcher->pushContext(connectorContext->context);
      connectorContext->interrupted = true;
    }
  }

  stopped = true;
}

TcpConnection TcpConnector::connect(const Ipv4Address& address, uint16_t port) {
  assert(dispatcher != nullptr);
  assert(context == nullptr);
  if (stopped) {
    throw InterruptedException();
  }

  std::string message;
  int connection = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (connection == -1) {
    message = "socket() failed, errno=" + std::to_string(errno);
  } else {
    sockaddr_in bindAddress;
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = 0;
    bindAddress.sin_addr.s_addr = INADDR_ANY;
    if (bind(connection, reinterpret_cast<sockaddr*>(&bindAddress), sizeof bindAddress) != 0) {
      message = "bind failed, errno=" + std::to_string(errno);
    } else {
      int flags = fcntl(connection, F_GETFL, 0);
      if (flags == -1 || fcntl(connection, F_SETFL, flags | O_NONBLOCK) == -1) {
        message = "fcntl() failed errno=" + std::to_string(errno);
      } else {
        sockaddr_in addressData;
        addressData.sin_family = AF_INET;
        addressData.sin_port = htons(port);
        addressData.sin_addr.s_addr = htonl(address.getValue());
        int result = ::connect(connection, reinterpret_cast<sockaddr *>(&addressData), sizeof addressData);
        if (result == -1) {
          if (errno == EINPROGRESS) {
            ConnectorContext connectorContext;
            connectorContext.context = dispatcher->getCurrentContext();
            connectorContext.interrupted = false;
            connectorContext.connection = connection;

            struct kevent event;
            EV_SET(&event, connection, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT | EV_CLEAR, 0, 0, &connectorContext);
            if (kevent(dispatcher->getKqueue(), &event, 1, NULL, 0, NULL) == -1) {
              message = "kevent() failed, errno=" + std::to_string(errno);
            } else {
              context = &connectorContext;
              dispatcher->dispatch();
              assert(dispatcher != nullptr);
              assert(connectorContext.context == dispatcher->getCurrentContext());
              assert(context == &connectorContext);
              context = nullptr;
              connectorContext.context = nullptr;
              if (connectorContext.interrupted) {
                throw InterruptedException();
              }

              struct kevent event;
              EV_SET(&event, connection, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, NULL);

              if (kevent(dispatcher->getKqueue(), &event, 1, NULL, 0, NULL) == -1) {
                message = "kevent() failed, errno=" + std::to_string(errno);
              } else {
                int retval = -1;
                socklen_t retValLen = sizeof(retval);
                int s = getsockopt(connection, SOL_SOCKET, SO_ERROR, &retval, &retValLen);
                if (s == -1) {
                  message = "getsockopt() failed, errno=" + std::to_string(errno);
                } else {
                  if (retval != 0) {
                    message = "connect failed; getsockopt retval =" + std::to_string(errno);
                  } else {
                    return TcpConnection(*dispatcher, connection);
                  }
                }
              }
            }
          }
        } else {
          return TcpConnection(*dispatcher, connection);
        }
      }
    }

    int result = close(connection);
    assert(result != -1);;
  }

  throw std::runtime_error("TcpConnector::connect, " + message);
}

}
