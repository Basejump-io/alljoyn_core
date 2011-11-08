/* bbjoin - will join any names on multipoint session port 26.*/

/******************************************************************************
 * Copyright 2009-2011, Qualcomm Innovation Center, Inc.
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
#include <qcc/StringUtil.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <vector>
#include <map>

#include <qcc/Debug.h>
#include <qcc/Environ.h>
#include <qcc/String.h>
#include <qcc/Thread.h>
#include <qcc/time.h>
#include <qcc/Util.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/MsgArg.h>
#include <alljoyn/version.h>

#include <Status.h>


#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;
using namespace ajn;

namespace org {
namespace alljoyn {
namespace alljoyn_test {
const char* InterfaceName1 = "org.alljoyn.signals.Interface";
const char* DefaultWellKnownName = "org.alljoyn.signals";
const char* ObjectPath = "/org/alljoyn/signals";
}
}
}

/** Static top level message bus object */
static BusAttachment* g_msgBus = NULL;
static Event g_discoverEvent;
static String g_wellKnownName = ::org::alljoyn::alljoyn_test::DefaultWellKnownName;
static bool g_acceptSession = true;
static bool g_stressTest = false;
static char* g_findPrefix = NULL;
static int g_sleepBeforeRejoin = 0;
SessionPort SESSION_PORT_MESSAGES_MP1 = 26;

static volatile sig_atomic_t g_interrupt = false;

static void SigIntHandler(int sig)
{
    g_interrupt = true;
}

class MyBusListener : public BusListener, public SessionPortListener, public SessionListener, public BusAttachment::JoinSessionAsyncCB {

  public:
    MyBusListener(BusAttachment* bus = NULL) : leaveSessionThread(bus, *this) { }

    class LeaveSessionThread : public Thread {
      public:
        LeaveSessionThread(BusAttachment* bus, MyBusListener& listener) : Thread("LSThread"), bus(bus), listener(listener) { }

      protected:
        ThreadReturn STDCALL Run(void* arg)
        {
            AsyncJoinCBCtx* ctx = reinterpret_cast<AsyncJoinCBCtx*>(arg);
            SessionId sessionId = ctx->id;
            String name = ctx->name;
            SessionOpts opts = ctx->opts;
            delete ctx;

            QCC_SyncPrintf("Calling LeaveSession(%u)\n", sessionId);
            QStatus status = bus->LeaveSession(sessionId);
            QCC_SyncPrintf("LeaveSession(%u) returned %s\n", sessionId, QCC_StatusText(status));

            if (status == ER_OK) {
                if (g_sleepBeforeRejoin) {
                    qcc::Sleep(g_sleepBeforeRejoin);
                }
                status = bus->JoinSessionAsync(name.c_str(), 26, &listener, opts, &listener, ::strdup(name.c_str()));
                if (status != ER_OK) {
                    QCC_LogError(status, ("JoinSessionAsync failed"));
                }
            } else {
                QCC_LogError(status, ("LeaveSession failed"));
            }
            return 0;
        }

      private:
        BusAttachment* bus;
        MyBusListener& listener;
    };

    struct AsyncJoinCBCtx {
        SessionId id;
        String name;
        SessionOpts opts;

        AsyncJoinCBCtx(SessionId id, const char* name, const SessionOpts& opts) : id(id), name(name), opts(opts) { }
    };

    bool AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts)
    {
        return g_acceptSession;
    }

    void SessionJoined(SessionPort sessionPort, SessionId sessionId, const char* joiner)
    {
        QCC_SyncPrintf("Session Established: joiner=%s, sessionId=%u\n", joiner, sessionId);
        QStatus status = g_msgBus->SetSessionListener(sessionId, this);
        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to SetSessionListener(%u)", sessionId));
        }
    }

    void FoundAdvertisedName(const char* name, TransportMask transport, const char* namePrefix)
    {
        QCC_SyncPrintf("FoundAdvertisedName(name=%s, transport=0x%x, prefix=%s)\n", name, transport, namePrefix);
        if (strcmp(name, g_wellKnownName.c_str()) != 0) {
            SessionOpts::TrafficType traffic = SessionOpts::TRAFFIC_MESSAGES;
            SessionOpts opts(traffic, true, SessionOpts::PROXIMITY_ANY, TRANSPORT_ANY);

            QStatus status = g_msgBus->JoinSessionAsync(name, 26, this, opts, this, ::strdup(name));
            if (ER_OK != status) {
                QCC_LogError(status, ("JoinSessionAsync(%s) failed \n", name));
                exit(1);
            }
        }
    }

    void JoinSessionCB(QStatus status, SessionId sessionId, const SessionOpts& opts, void* context)
    {
        const char* name = reinterpret_cast<const char*>(context);

        if (status == ER_OK) {
            QCC_SyncPrintf("JoinSessionAsync succeeded. SessionId=%u\n", sessionId);
        } else {
            QCC_LogError(status, ("JoinSessionAsych failed"));
            QCC_SyncPrintf("JoinSession failed with %s\n", QCC_StatusText(status));
        }

        /* LeaveSession on a non-callback thread since this call can block when tx queues are full */
        if ((status == ER_OK) && g_stressTest) {
            leaveSessionThread.Join();
            status = leaveSessionThread.Start(new AsyncJoinCBCtx(sessionId, name, opts));
            if (status != ER_OK) {
                QCC_LogError(status, ("LeaveSessionThread::Start failed"));
                exit(1);
            }
        }
        free(context);
    }

    void LostAdvertisedName(const char* name, const TransportMask transport, const char* prefix)
    {
        QCC_SyncPrintf("LostAdvertisedName(name=%s, transport=0x%x,  prefix=%s)\n", name, transport, prefix);
    }

    void NameOwnerChanged(const char* name, const char* previousOwner, const char* newOwner)
    {
        QCC_SyncPrintf("NameOwnerChanged(%s, %s, %s)\n",
                       name,
                       previousOwner ? previousOwner : "null",
                       newOwner ? newOwner : "null");
    }

    void SessionLost(SessionId sessid)
    {
        QCC_SyncPrintf("Session Lost  %u\n", sessid);
    }

  private:
    LeaveSessionThread leaveSessionThread;
};

static void usage(void)
{
    printf("Usage: bbjoin \n\n");
    printf("Options:\n");
    printf("   -h           = Print this help message\n");
    printf("   -n <name>    = Well-known name to advertise\n");
    printf("   -r           = Reject incoming joinSession attempts\n");
    printf("   -s           = Stress test. Continous leave/join\n");
    printf("   -f <prefix>  = FindAdvertisedName prefix\n");
    printf("   -b           = Advertise over Bluetooth (enables selective advertising)\n");
    printf("   -t           = Advertise over TCP (enables selective advertising)\n");
    printf("   -dj <ms>     = Number of ms to delay between leaving and re-joining\n");
    printf("\n");
}

/** Main entry point */
int main(int argc, char** argv)
{
    QStatus status = ER_OK;
    uint32_t transportOpts = 0;

#ifdef _WIN32
    WSADATA wsaData;
    WORD version = MAKEWORD(2, 0);
    int error = WSAStartup(version, &wsaData);
#endif

    printf("AllJoyn Library version: %s\n", ajn::GetVersion());
    printf("AllJoyn Library build info: %s\n", ajn::GetBuildInfo());

    /* Install SIGINT handler */
    signal(SIGINT, SigIntHandler);

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp("-h", argv[i])) {
            usage();
            exit(0);
        } else if (0 == strcmp("-n", argv[i])) {
            ++i;
            if (i == argc) {
                printf("option %s requires a parameter\n", argv[i - 1]);
                usage();
                exit(1);
            } else {
                g_wellKnownName = argv[i];
            }
        } else if (0 == strcmp("-r", argv[i])) {
            g_acceptSession = false;
        } else if (0 == strcmp("-s", argv[i])) {
            g_stressTest = true;
        } else if (0 == strcmp("-f", argv[i])) {
            g_findPrefix = argv[++i];
        } else if (0 == strcmp("-b", argv[i])) {
            transportOpts |= TRANSPORT_BLUETOOTH;
        } else if (0 == strcmp("-t", argv[i])) {
            transportOpts |= TRANSPORT_WLAN;
        } else if (0 == strcmp("-dj", argv[i])) {
            g_sleepBeforeRejoin = qcc::StringToU32(argv[++i], 0);
        } else {
            status = ER_FAIL;
            printf("Unknown option %s\n", argv[i]);
            usage();
            exit(1);
        }
    }

    /* If no transport option was specifie, then make session options very open */
    if (transportOpts == 0) {
        transportOpts = TRANSPORT_ANY;
    }

    /* Get env vars */
    Environ* env = Environ::GetAppEnviron();
    qcc::String clientArgs = env->Find("DBUS_STARTER_ADDRESS");

    if (clientArgs.empty()) {
#ifdef _WIN32
        clientArgs = env->Find("BUS_ADDRESS", "tcp:addr=127.0.0.1,port=9955");
#else
        clientArgs = env->Find("BUS_ADDRESS", "unix:abstract=alljoyn");
#endif
    }

    /* Create message bus */
    g_msgBus = new BusAttachment("bbjoin", true);

    /* Start the msg bus */
    if (ER_OK == status) {
        status = g_msgBus->Start();
    } else {
        QCC_LogError(status, ("BusAttachment::Start failed"));
    }

    /* Connect to the daemon */
    status = g_msgBus->Connect(clientArgs.c_str());

    MyBusListener myBusListener(g_msgBus);
    g_msgBus->RegisterBusListener(myBusListener);

    /* Register local objects and connect to the daemon */
    if (ER_OK == status) {

        /* Create session opts */
        SessionOpts optsmp(SessionOpts::TRAFFIC_MESSAGES, true,  SessionOpts::PROXIMITY_ANY, transportOpts);

        /* Create a session for incoming client connections */
        status = g_msgBus->BindSessionPort(SESSION_PORT_MESSAGES_MP1, optsmp, myBusListener);
        if (status != ER_OK) {
            QCC_LogError(status, ("BindSessionPort failed"));
        }

        /* Request a well-known name */
        QStatus status = g_msgBus->RequestName(g_wellKnownName.c_str(), DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE);
        if (status != ER_OK) {
            status = (status == ER_OK) ? ER_FAIL : status;
            QCC_LogError(status, ("RequestName(%s) failed. ", g_wellKnownName.c_str()));
            return status;
        }

        /* Begin Advertising the well-known name */
        status = g_msgBus->AdvertiseName(g_wellKnownName.c_str(), transportOpts);
        if (ER_OK != status) {
            status = (status == ER_OK) ? ER_FAIL : status;
            QCC_LogError(status, ("Sending org.alljoyn.Bus.Advertise failed "));
            return status;
        }

        status = g_msgBus->FindAdvertisedName(g_findPrefix ? g_findPrefix : "com");
        if (status != ER_OK) {
            status = (status == ER_OK) ? ER_FAIL : status;
            QCC_LogError(status, ("FindAdvertisedName failed "));
        }
    }

    while (g_interrupt == false) {
        qcc::Sleep(100);
    }

    /* Clean up msg bus */
    if (g_msgBus) {
        BusAttachment* deleteMe = g_msgBus;
        g_msgBus = NULL;
        delete deleteMe;
    }

    printf("\n %s exiting with status %d (%s)\n", argv[0], status, QCC_StatusText(status));

    return (int) status;
}
