/**
 * @file
 * NullTransport implementation
 */

/******************************************************************************
 * Copyright 2012, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/
#include <qcc/platform.h>

#include <list>

#include <errno.h>
#include <qcc/Socket.h>
#include <qcc/SocketStream.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/Util.h>

#include <alljoyn/BusAttachment.h>

#include "BusInternal.h"
#include "RemoteEndpoint.h"
#include "NullTransport.h"

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

const char* NullTransport::TransportName = "null";

DaemonLauncher* NullTransport::daemonLauncher;

BusAttachment* NullTransport::daemonBus;

/*
 * The null endpoint simply moves messages between the daemon router to the client router and lets
 * the routers handle it from there. The only wrinkle is that messages forwarded to the daemon may
 * need to be encrypted because in the non-bundled case encryption is done in _Message::Deliver()
 * and that method does not get called in this case.
 */
class NullEndpoint : public BusEndpoint {

  public:

    NullEndpoint(BusAttachment& clientBus, BusAttachment& daemonBus);

    ~NullEndpoint();

    QStatus PushMessage(Message& msg)
    {
        QStatus status = ER_OK;
        /*
         * In the un-bundled daemon case messages store the name of the endpoint they were received
         * on. As far as the client and daemon routers are concerned the message was received from
         * this endpoint so we must set the received name to the unique name of this endpoint.
         */
        msg->rcvEndpointName = uniqueName;
        /*
         * If the message came from the daemon forward it to the client and visa versa.
         */
        if (msg->bus == &daemonBus) {
            msg->bus = &clientBus;
            status = clientBus.GetInternal().GetRouter().PushMessage(msg, *this);
        } else {
            /*
             * Messages we are sending to the daemon may need to be encrypted.
             */
            if (msg->encrypt) {
                status = msg->EncryptMessage();
            }
            if (status == ER_OK) {
                msg->bus = &daemonBus;
                status = daemonBus.GetInternal().GetRouter().PushMessage(msg, *this);
            } else if (status == ER_BUS_AUTHENTICATION_PENDING) {
                status = ER_OK;
            }
        }
        return status;
    }

    const qcc::String& GetUniqueName() const { return uniqueName; }

    uint32_t GetUserId() const { return -1; }
    uint32_t GetGroupId() const { return -1; }
    uint32_t GetProcessId() const { return -1; }
    bool SupportsUnixIDs() const { return false; }
    bool AllowRemoteMessages() { return true; }

    BusAttachment& clientBus;
    BusAttachment& daemonBus;

    qcc::String uniqueName;
};

NullEndpoint::NullEndpoint(BusAttachment& clientBus, BusAttachment& daemonBus) :
    BusEndpoint(ENDPOINT_TYPE_NULL),
    clientBus(clientBus),
    daemonBus(daemonBus)
{
    /*
     * We get a unique name for this endpoint the usual way.
     */
    uniqueName = daemonBus.GetInternal().GetRouter().GenerateUniqueName();
    /*
     * Register the null endpont with both routers.
     */
    clientBus.GetInternal().GetRouter().RegisterEndpoint(*this, false);
    daemonBus.GetInternal().GetRouter().RegisterEndpoint(*this, false);
}

NullEndpoint::~NullEndpoint()
{
    clientBus.GetInternal().GetRouter().UnregisterEndpoint(*this);
    daemonBus.GetInternal().GetRouter().UnregisterEndpoint(*this);
}

NullTransport::NullTransport(BusAttachment& bus) : bus(bus), running(false), endpoint(NULL)
{
}

NullTransport::~NullTransport()
{
}

QStatus NullTransport::Start()
{
    running = true;
    return ER_OK;
}

QStatus NullTransport::Stop(void)
{
    running = false;
    if (daemonLauncher) {
        daemonLauncher->Stop();
    }
    return ER_OK;
}

QStatus NullTransport::Join(void)
{
    if (daemonLauncher) {
        daemonLauncher->Join();
    }
    return ER_OK;
}

QStatus NullTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, std::map<qcc::String, qcc::String>& argMap) const
{
    outSpec = inSpec;
    return ER_OK;
}

QStatus NullTransport::Connect(const char* connectSpec, const SessionOpts& opts, BusEndpoint** newep)
{
    QStatus status = ER_OK;

    if (!running) {
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }
    if (!daemonLauncher) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }
    if (!daemonBus) {
        status = daemonLauncher->Start(daemonBus);
    }
    if (status == ER_OK) {
        assert(daemonBus);
        endpoint = new NullEndpoint(bus, *daemonBus);
        if (newep) {
            *newep = endpoint;
        }
    }
    return status;
}

QStatus NullTransport::Disconnect(const char* connectSpec)
{
    delete endpoint;
    endpoint = NULL;
    return ER_OK;
}

void NullTransport::RegisterDaemonLauncher(DaemonLauncher* launcher)
{
    daemonLauncher = launcher;
}

} // namespace ajn
